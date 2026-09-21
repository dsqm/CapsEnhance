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
   本文件的部分内容派生自 AltSnap
     AltSnap: Copyright (C) 2015 Stefan Sundin
              Modified By Raymond Gillibert in 2022
              https://github.com/RamonUnch/AltSnap
   AltSnap 以 GPL-3.0-or-later 授权；本项目同样以 GPL-3.0-or-later 授权。
   ------------------------------------------------------------------ */

/* ================= Caps + 左键拖动挪窗口（src/drag.c） =================
   按住 Caps 时按下左键：把**光标下的顶层窗口**接管过来跟着鼠标走 ——
   也就是 AltSnap 用 Alt+左键做的事（本项目把修饰键换成了 Caps）。
   交互与实现思路借鉴 AltSnap（hooks.c 的鼠标钩子 + 移动循环），结构按本项目来：
   钩子只做"判定 + 记录 + 唤醒"，`SetWindowPos` 在专用线程上执行 ——
   它要向对方窗口过程发 WM_WINDOWPOSCHANGING/CHANGED，对方卡住时同步调用能
   一直等下去，落在钩子线程上就是"钩子被静默摘除"（见 act.c 顶部）。

   与 AltSnap 的差异（刻意收窄，没有全部搬过来）：
     · 不做 Aero Snap / 磁吸（snap.c / zones.c 那一整套）；
     · 不做拖动时的透明（AltSnap 默认 MoveTrans=255 = 关）与双击最大化；
     · 不穿透点击：Caps+左键按下去就是"拖"，不再传给目标程序（AltSnap 默认
       PiercingClick=0，同为不穿透）；
     · 黑名单分**两份**（[DragWindowBlacklist] 按窗口类名 / [DragProcessBlacklist]
       按进程名），与 AltSnap 默认黑名单的 Windows / Processes 两条一一对应，
       默认值也逐条取自它，各自独立缺省回落。AltSnap 还支持"标题"维度，本项目
       不支持：取另一个进程的标题要发 WM_GETTEXT（跨进程 IPC），而判定发生在
       鼠标钩子上 —— 钩子上绝不能做跨进程消息。AltSnap 的默认条目每条都带类名
       或进程名，所以默认语义仍然完整。

   ★ 拖动的"最新位置 wins"：钩子每帧只做一次临界区写 + SetEvent（不排队、不
   阻塞），执行线程醒来取**最新**的点应用一次 —— 多余的中间帧自然合并，与
   AltSnap 用 MoveRate/RezTimer 做节流是同一个目的，但这里用"最新点 wins"。
   （每帧进一次临界区是刻意的：持续滚动那次的"数据/标志两步写"竞态，用锁从
   根上消灭；拖动一拍一次，开销远小于一次 SetWindowPos。） */

#include "cap.h"
#include "lane.h"

#include <string.h>

/* ---- 通道参数 ---- */
#define DRAG_SETTLE_MS   20    /* 等待/轮询的步长（毫秒） */
#define DRAG_RESTORE_MAX 200   /* 等"最大化还原"落地的上限，等不到就收手 */

/* 拖出阈值（对齐 AltSnap 的 `DragThreshold=1` + `DragOutThresholdPx=20`）：
   **只对最大化窗口**生效 —— 按下之后要先拖出 DRAG_OUT_THRESHOLD_PX 像素，才真正
   开始“还原 + 跟随”。为什么必须有：最大化窗口一还原就变小，如果“按下即还原”，
   那么 Caps+左键**点一下**（手没动）也会把窗口缩小 —— 这是本功能最刺眼的问题。
   （AltSnap 的 DragThreshold：0=不设阈值；1=只对最大化/已吸附窗口设（默认）；
   2=总是设；3=缩放时也设。我们还没有“吸附”，所以等价于 1。普通窗口按下即跟手。） */
#define DRAG_OUT_THRESHOLD_PX 20

static DWORD WINAPI DragThread(LPVOID p);   /* 前置声明：LaneStart 要先拿到它 */

/* ---- 状态（钩子线程写、执行线程读，全部在临界区里过手） ----
   钩子路径上每次鼠标移动都要过一次临界区：这是刻意的 —— 持续滚动那次的
   "写序竞态"（数据/标志两步写）用锁从根上消灭；拖动一拍一次，开销远小于
   一次 SetWindowPos。 */
static HWND  s_hwnd = NULL;      /* 正在拖的窗口（只做 IsWindow 校验，不持有引用） */
static POINT s_pressPt;          /* 按下点：只用于判定"有没有拖出阈值"，不参与落点计算 */
static BOOL  s_waitDragOut = FALSE; /* TRUE = 还在等拖出阈值（仅最大化窗口） */
static POINT s_wndTL;            /* 抓取基准：按下（或还原完成）那一刻的窗口左上角 */
static int   s_w = 0, s_h = 0;   /* 抓取时的尺寸（拖动不改尺寸） */
static POINT s_origin;           /* 抓取那一刻的光标 */
static POINT s_cur;              /* 最新光标（钩子每帧写） */
static int   s_ticks = 0;        /* 诊断：本次拖动应用了多少拍 */
static BOOL  s_pending = FALSE;  /* 有还没应用的新光标 */
static BOOL  s_endReq = FALSE;   /* 钩子已见到抬起：应用完最后一点就收手 */
static BOOL  s_needRestore = FALSE; /* 按下时是最大化：先等还原落地再取基准矩形 */
static volatile LONG s_active = 0;   /* ★ 最后置：执行线程只信它 */
/* 临界区 / 事件 / 线程 / 停止位四件套交给 lane.c（见 lane.h）—— 这一份原先与
   act.c 的三条、ime.c 的一条逐行相同，其中"join 超时后不许拆锁与事件"那条
   判断最危险，现在只写在 lane.c 里。
   本文件只有这一条车道，所以钩子与线程都直接用 s_lane：业务状态一律挂在
   s_lane.cs 这把锁下（线程形参因此用不上，只满足 LaneStart 的签名）。 */
static Lane s_lane;

/* 纯函数：落点 = 抓取基准的左上角 + 光标位移。单测覆盖（含负位移）。 */
void DragComputeTopLeft(POINT wndTL, POINT from, POINT to, POINT* out) {
    out->x = wndTL.x + (to.x - from.x);
    out->y = wndTL.y + (to.y - from.y);
}

/* 是否“拖出了”阈值：**任一轴**超过即算（与 AltSnap 的 `IsPtDragOut` 同义 ——
   它是个方框判定而不是半径）。用方框是因为用户拖动方向不确定；用半径会让斜向
   拖动比正上下左右更难触发。 */
static BOOL DragOutReached(POINT from, POINT to, int px) {
    return (to.x > from.x + px) || (to.x < from.x - px) ||
           (to.y > from.y + px) || (to.y < from.y - px);
}

/* ---- 起停（act.c 的 ActStart / ActStop 调用） ---- */
void DragLaneStart(void) {
    if (LaneLive(&s_lane)) return;
    memset(&s_wndTL, 0, sizeof(s_wndTL));
    memset(&s_origin, 0, sizeof(s_origin));
    memset(&s_cur, 0, sizeof(s_cur));
    memset(&s_pressPt, 0, sizeof(s_pressPt));
    s_w = s_h = 0;
    s_ticks = 0;
    s_pending = s_endReq = s_needRestore = s_waitDragOut = FALSE;
    InterlockedExchange(&s_active, 0);
    /* 起不来就静默不可用（钩子入口都先问 LaneLive）—— 回滚与失败语义在 lane.c。 */
    LaneStart(&s_lane, DragThread);
}

void DragLaneStop(void) {
    LaneStop(&s_lane, 1000);   /* 一拍有界（DRAG_SETTLE_MS），1s 足够 */
}

/* ---- 钩子入口（主线程，鼠标输入的关键路径） ---- */
BOOL DragBegin(POINT pt) {
    if (!LaneLive(&s_lane) || g_state.paused) return FALSE;

    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) return FALSE;
    hwnd = GetAncestor(hwnd, GA_ROOT);          /* 内容子窗口 -> 顶层窗口 */
    if (!hwnd || hwnd == GetShellWindow()) return FALSE;
    if (hwnd == g_mainWnd) return FALSE;        /* 自己的窗口（托盘菜单）不挪 */
    if (WinIsShellClass(hwnd)) return FALSE;    /* 桌面 / 任务栏 / 通知区 */
    if (ConfigIsDragBlacklisted(hwnd)) return FALSE;
    if (!IsWindowVisible(hwnd)) return FALSE;
    if (IsIconic(hwnd)) return FALSE;           /* 最小化：拖不出可见效果，点击照旧 */

    /* 全屏窗口不拖（AltSnap 默认 FullScreen=0：整屏矩形 = 全屏程序，拖它没意义）。
       判定抽在 winmove.c 的 WinIsFullscreen 里：C+Q 也用它，两边标准必须同一条 —— 
       否则会出现"拖动不让动、C+Q 却去动"这种自相矛盾的行为。 */
    if (WinIsFullscreen(hwnd)) return FALSE;

    const BOOL wasMax = IsZoomed(hwnd) != FALSE;

    /* 普通窗口的抓取基准（尺寸 + 左上角）在这里就取好：进临界区之后再取的话，
       取不到就只能带着没释的锁 return —— 那正是难查的死锁。最大化窗口不需要
       （等拖出阈值还原之后再量），保持 rc 为零。 */
    RECT rc;
    memset(&rc, 0, sizeof(rc));
    if (!wasMax && !GetWindowRect(hwnd, &rc)) return FALSE;

    EnterCriticalSection(&s_lane.cs);
    s_hwnd = hwnd;
    s_pressPt = pt;                  /* ★ 阈值判定专用（落点用的 s_origin 会被重锚） */
    s_origin = pt;
    s_cur = pt;
    s_endReq = FALSE;
    s_ticks = 0;
    if (wasMax) {
        /* 最大化：先**什么都不做**，等拖出阈值再还原 —— 否则“按下即还原”会让
           Caps+左键点一下就把窗口缩小。 */
        s_waitDragOut = TRUE;
        s_needRestore = FALSE;
        s_pending = FALSE;
    } else {
        s_waitDragOut = FALSE;
        s_needRestore = FALSE;
        s_pending = TRUE;            /* 普通窗口：先算一次落点（哪怕光标还没动） */
    }
    s_w = s_h = 0;
    s_wndTL.x = s_wndTL.y = 0;
    if (!wasMax) {
        s_w = (int)(rc.right - rc.left);
        s_h = (int)(rc.bottom - rc.top);
        s_wndTL.x = rc.left;
        s_wndTL.y = rc.top;
    }
    InterlockedExchange(&s_active, 1);   /* ★ 最后置：执行线程只信它 */
    LeaveCriticalSection(&s_lane.cs);
    if (g_recording.load()) {
        char cls8[64];
        WCHAR wcls[64] = L"";
        GetClassNameW(hwnd, wcls, 64);
        WideCharToMultiByte(CP_ACP, 0, wcls, -1, cls8, 64, NULL, NULL);
        cls8[63] = 0;
        CapLog("  [拖拽] 开始 hwnd=%p cls=%s rect[%ld,%ld..%ld,%ld] 起点(%ld,%ld)%s",
               (void*)hwnd, cls8[0] ? cls8 : "?",
               (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom,
               (long)pt.x, (long)pt.y,
               wasMax ? " 最大化→等拖出阈值才还原" : "");
    }
    LanePoke(&s_lane);
    return TRUE;
}

void DragMove(POINT pt) {
    if (!LaneLive(&s_lane)) return;
    EnterCriticalSection(&s_lane.cs);
    if (InterlockedOr(&s_active, 0)) {
        s_cur = pt;
        s_pending = TRUE;
    }
    LeaveCriticalSection(&s_lane.cs);
    LanePoke(&s_lane);
}

void DragEnd(void) {
    if (!LaneLive(&s_lane)) return;
    EnterCriticalSection(&s_lane.cs);
    s_endReq = TRUE;                     /* 应用完最后一点再收手 */
    s_pending = TRUE;
    LeaveCriticalSection(&s_lane.cs);
    LanePoke(&s_lane);
}

BOOL DragActive(void) {
    return LaneLive(&s_lane) && InterlockedOr(&s_active, 0) != 0;
}

/* ---- 执行线程 ---- */
static DWORD WINAPI DragThread(LPVOID p) {
    (void)p;
    BOOL running = FALSE;
    for (;;) {
        if (LaneStopped(&s_lane)) break;
        WaitForSingleObject(s_lane.ev, 1000);      /* 有新位置 / 结束 / 1s 兜底 */
        if (LaneStopped(&s_lane)) break;

        if (!running) {
            /* 空闲：先看状态再看事件，避免"事件已被上一轮吃掉"时白等 1s。 */
            if (!InterlockedOr(&s_active, 0)) continue;
            if (LaneStopped(&s_lane)) break;
            running = TRUE;
        }

        /* 读一拍的状态（窗口 / 锚点 / 尺寸在锁内取，免得和 Begin/End 撕裂） */
        BOOL endReq = FALSE, pending = FALSE, needRestore = FALSE, waitOut = FALSE;
        HWND hwnd = NULL;
        POINT tl, to, org, press;
        int w = 0, h = 0, ticks = 0;
        /* 还原前的矩形（只在"最大化窗口拖过阈值"这一拍里有用，见阶段 0/1）：
           只有执行线程读写，不进锁、不跨线程。 */
        RECT restoreFrom;
        BOOL haveRestoreFrom = FALSE;
        EnterCriticalSection(&s_lane.cs);
        hwnd = s_hwnd;
        endReq = s_endReq;
        pending = s_pending;
        needRestore = s_needRestore;
        waitOut = s_waitDragOut;
        to = s_cur;
        press = s_pressPt;
        tl = s_wndTL;
        org = s_origin;
        w = s_w;
        h = s_h;
        ticks = s_ticks;
        LeaveCriticalSection(&s_lane.cs);

        /* 阶段 0：等“拖出阈值”（仅最大化窗口）。没越过就**完全不动它**，包括不还原 ——
           于是 Caps+左键点一下（手没动）不会把最大化窗口缩小；松手时直接收手，
           窗口原样不动（点击已被吞掉，不会漏给目标程序）。 */
        if (waitOut) {
            if (!DragOutReached(press, to, DRAG_OUT_THRESHOLD_PX)) {
                if (!endReq) continue;          /* 阈值内：继续等（或等松手） */
                if (g_recording.load())
                    CapLog("  [拖拽] 结束（未越过拖出阈值，窗口未动）");
                EnterCriticalSection(&s_lane.cs);
                s_hwnd = NULL;
                s_pending = FALSE;
                s_endReq = FALSE;
                s_waitDragOut = FALSE;
                LeaveCriticalSection(&s_lane.cs);
                InterlockedExchange(&s_active, 0);
                running = FALSE;
                continue;
            }
            /* 越过阈值：现在才异步还原，下一拍等它落地（阶段 1 的有界轮询）。
               先把"还原前那份占满整屏的矩形"记下来 —— 阶段 1 要用它判断还原
               **真的落地了**（只等 IsZoomed 不够，见那里）。取不到就没有这条
               判据，退回只看 IsZoomed 的老行为。 */
            haveRestoreFrom = (GetWindowRect(hwnd, &restoreFrom) != FALSE);
            PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            EnterCriticalSection(&s_lane.cs);
            s_waitDragOut = FALSE;
            s_needRestore = TRUE;
            s_pending = TRUE;
            LeaveCriticalSection(&s_lane.cs);
            needRestore = TRUE;
            pending = TRUE;
            if (g_recording.load())
                CapLog("  [拖拽] 越过拖出阈值(%dpx) -> 还原+移动", DRAG_OUT_THRESHOLD_PX);
        }

        /* 窗口没了：收手（ Begin/End 之外唯一的收手路径） */
        if (!hwnd || !IsWindow(hwnd)) {
            EnterCriticalSection(&s_lane.cs);
            s_hwnd = NULL;
            s_pending = FALSE;
            s_endReq = FALSE;
            LeaveCriticalSection(&s_lane.cs);
            InterlockedExchange(&s_active, 0);
            running = FALSE;
            continue;
        }

        /* 阶段 1：等"最大化还原"落地。SC_RESTORE 是 PostMessage 发的（异步），
           不能在钩子线程里等，所以放到这里**有界轮询**：等到了就把抓取基准换成
           还原后的矩形、并把光标基准重新锚到"现在"（还原期间鼠标动过的距离不再
           叠加，否则窗口会跳）；等不到就收手 —— 宁可这次不拖，也别挪去错的地方。 */
        if (needRestore) {
            int waited = 0;
            while (IsZoomed(hwnd) && waited < DRAG_RESTORE_MAX) {
                Sleep(DRAG_SETTLE_MS);
                waited += DRAG_SETTLE_MS;
            }
            EnterCriticalSection(&s_lane.cs);
            s_needRestore = FALSE;
            LeaveCriticalSection(&s_lane.cs);
            if (IsZoomed(hwnd)) {              /* 拒绝还原：本次拖动作废 */
                if (g_recording.load())
                    CapLog("  [拖拽] 最大化还原未落地，本次作废");
                InterlockedExchange(&s_active, 0);
                running = FALSE;
                continue;
            }
            RECT rc;
            if (!GetWindowRect(hwnd, &rc)) {
                InterlockedExchange(&s_active, 0);
                running = FALSE;
                continue;
            }
            /* ★ 只等 IsZoomed 变假**不算还原落地**：还原是"先清样式、再把窗口挪回
               普通位置与尺寸"两步，而我们是从**别的线程**读它的 —— 中间那个瞬间
               IsZoomed 已经为假、矩形还是最大化时那份占满整屏的。拿它当抓取基准
               的话，整次拖拽的落点就停在屏幕角上（CI 上"AF: 还原后位置随光标"
               挂掉即是此因；以前那个"多跑一段阈值位移"的缺陷反而把症状盖住了）。
               所以继续等到矩形**离开了还原前那一份**为止，同样有界。 */
            if (haveRestoreFrom) {
                int waited2 = 0;
                BOOL alive = TRUE;
                while (EqualRect(&rc, &restoreFrom) && waited2 < DRAG_RESTORE_MAX) {
                    Sleep(DRAG_SETTLE_MS);
                    waited2 += DRAG_SETTLE_MS;
                    if (!GetWindowRect(hwnd, &rc)) { alive = FALSE; break; }
                }
                if (!alive || EqualRect(&rc, &restoreFrom)) {
                    /* 窗口没了，或还原后矩形一点没变 = 基准不可信。
                       与"还原未落地"同一处理。 */
                    if (g_recording.load() && alive)
                        CapLog("  [拖拽] 还原后矩形未变，本次作废");
                    InterlockedExchange(&s_active, 0);
                    running = FALSE;
                    continue;
                }
            }
            EnterCriticalSection(&s_lane.cs);
            s_wndTL.x = rc.left;
            s_wndTL.y = rc.top;
            s_w = (int)(rc.right - rc.left);
            s_h = (int)(rc.bottom - rc.top);
            s_origin = to;                     /* 重新锚定：从"现在"开始跟随 */
            org = to;                          /* 本地快照必须同步换：阶段 2 用的是
                                                  org，不换就等于把刚抹掉的"拖出阈值
                                                  位移"又加回落点（窗口先跳一下再回来） */
            tl = s_wndTL;
            w = s_w; h = s_h;
            LeaveCriticalSection(&s_lane.cs);
        }

        /* 阶段 2：落点 = 抓取基准左上角 + 光标位移，应用一次（有新点或要收手才算） */
        if (pending || endReq) {
            POINT dst;
            DragComputeTopLeft(tl, org, to, &dst);
            /* ASYNCWINDOWPOS：对方线程卡住不拖住执行线程（见文件头）。 */
            SetWindowPos(hwnd, NULL, (int)dst.x, (int)dst.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_ASYNCWINDOWPOS);
        }

        EnterCriticalSection(&s_lane.cs);
        s_pending = FALSE;
        if (s_endReq) {
            if (g_recording.load())
                CapLog("  [拖拽] 结束 拍数=%d", ticks);
            s_hwnd = NULL;
            s_pending = FALSE;
            s_endReq = FALSE;
            s_waitDragOut = FALSE;
            s_needRestore = FALSE;
            InterlockedExchange(&s_active, 0);
            running = FALSE;
        }
        LeaveCriticalSection(&s_lane.cs);
    }
    return 0;
}
