#ifndef LANE_H
#define LANE_H

#include <windows.h>

/* ================= 工作车道底座（src/lane.c） =================
   "把会阻塞的事搬到后台线程"这个模式在本项目里出现了五次：
     act.c 的动作队列线程 / 滚轮补格线程 / 持续滚动线程，
     drag.c 的拖拽线程，ime.c 的输入法线程。
   五份代码的骨架逐行相同 —— 临界区 + 自动复位事件 + 线程句柄 + 停止标志 +
   带超时的 join + 起不来的回滚 —— 而其中"join 超时之后**绝不能**拆掉线程
   仍在使用的锁与事件"是最危险的一条判断。抽在这里之后它只写一遍。

   **不变量（改本文件前先读）**：
     · `LaneStopped()` 是工作线程**唯一**该看的停止判据（内部是 Interlocked，
       与主线程的写入构成可见性）。别自己再置一份 stop 标志。
     · 工作线程只允许通过 `LanePoke()` 被唤醒；它的等待一律带 1s 超时兜底
       （防"事件被前一轮吃掉"导致永久挂住）。
     · `LaneJoin` 超时 = 线程还活着：此时**不销毁** cs 与 ev，只把 `zombie`
       置位，之后 `LaneStart` 一律拒绝（对仍被持有的锁重做 InitializeCriticalSection
       是未定义行为）。进程退出前不回收这几个内核对象是刻意的 —— 代价最小。
     · 停止是**单向**的：`LaneStopRequest` 之后必须 `LaneJoin` 才算收干净；
       想再起一条就再 `LaneStart`（zombie 为假时合法，重启流程会用到）。

   线程函数签名用 `LPTHREAD_START_ROUTINE`，形参就是本车道的 `Lane*`。 */

typedef struct Lane {
    CRITICAL_SECTION cs;      /* 车道自己的锁：队列/状态都挂在这把锁下 */
    HANDLE ev;                /* 自动复位：一次投递一次唤醒              */
    HANDLE thread;
    volatile LONG stop;
    BOOL init;                /* 已起：Stop/Join/Poke 只在这个状态下动作 */
    BOOL zombie;              /* join 超时过：禁止重起（见上面不变量）    */
} Lane;

/* 起线程。起不来（或上次是 zombie）返回 FALSE，锁与事件已回滚干净。 */
BOOL LaneStart(Lane* L, LPTHREAD_START_ROUTINE fn);

void LaneStopRequest(Lane* L);            /* 置停 + 唤醒；不阻塞、不等        */
BOOL LaneJoin(Lane* L, DWORD ms);         /* TRUE = 已退出并销毁；FALSE = 超时 */
void LaneStop(Lane* L, DWORD ms);         /* 上面两个连起来（常见用法）        */

BOOL LaneStopped(Lane* L);                /* 工作线程的停止判据               */
BOOL LaneLive(Lane* L);                   /* 是否已起（投递入口先问它）        */
void LanePoke(Lane* L);                   /* 唤醒一次                         */

#endif /* LANE_H */
