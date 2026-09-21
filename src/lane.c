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

#include "lane.h"

/* 完整取舍见 lane.h 顶部。本文件只做那五步骨架，不含任何业务状态。 */

BOOL LaneStart(Lane* L, LPTHREAD_START_ROUTINE fn) {
    if (!L || !fn) return FALSE;
    if (L->zombie) return FALSE;   /* 上次超时没收干净：重起会碰到活线程的锁 */
    if (L->init)   return TRUE;
    InterlockedExchange(&L->stop, 0);
    InitializeCriticalSection(&L->cs);
    L->ev = CreateEventW(NULL, FALSE, FALSE, NULL);   /* 自动复位：一拍一唤醒 */
    L->thread = L->ev ? CreateThread(NULL, 0, fn, L, 0, NULL) : NULL;
    if (!L->ev || !L->thread) {
        if (L->thread) { CloseHandle(L->thread); L->thread = NULL; }
        if (L->ev)     { CloseHandle(L->ev);     L->ev = NULL; }
        DeleteCriticalSection(&L->cs);
        return FALSE;
    }
    L->init = TRUE;
    return TRUE;
}

void LaneStopRequest(Lane* L) {
    if (!L || !L->init) return;
    InterlockedExchange(&L->stop, 1);
    SetEvent(L->ev);               /* 别让它停在 WaitForSingleObject 里 */
}

BOOL LaneJoin(Lane* L, DWORD ms) {
    if (!L || !L->init) return TRUE;
    if (L->thread) {
        if (WaitForSingleObject(L->thread, ms) != WAIT_OBJECT_0) {
            /* 线程还活着：**绝不能拆它仍在用的共享对象**（见 lane.h 不变量）。 */
            L->zombie = TRUE;
            return FALSE;
        }
        CloseHandle(L->thread);
        L->thread = NULL;
    }
    if (L->ev) { CloseHandle(L->ev); L->ev = NULL; }
    DeleteCriticalSection(&L->cs);
    L->init = FALSE;
    return TRUE;
}

void LaneStop(Lane* L, DWORD ms) {
    LaneStopRequest(L);
    LaneJoin(L, ms);
}

BOOL LaneStopped(Lane* L) {
    if (!L) return TRUE;
    return InterlockedCompareExchange(&L->stop, 0, 0) != 0;
}

BOOL LaneLive(Lane* L) { return L && L->init; }

void LanePoke(Lane* L) {
    if (L && L->ev) SetEvent(L->ev);
}
