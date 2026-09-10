#include "cap.h"

/* ================= 动作执行线程 =================
   为什么需要它（这不是"优化"，是在修一个架构级缺陷）：

   WH_KEYBOARD_LL 的回调**在安装它的那个线程**上执行，而钩子由主线程安装
   （dwThreadId = 0 只表示"全局钩子"，不表示"系统线程"）。一次词跳在主线程
   上可能连续阻塞数百毫秒：
     · SendArrows 逐格发送   最多 256 步 x ~13ms  ≈ 3.3s（默认配置约 0.4s）
     · SendNativeWordJump    4 x COMBO_GAP(40ms)  = 160ms
     · NavJump 跨进程同步 IPC SendMsg(SMTO_BLOCK) 单次最长 300ms
     · UIA 读取 + 回读校验   COM 往返（另有单独缓存，见 nav.c）
   主线程被占住的这段时间，**系统级键盘输入无法被钩子处理** —— 表现为全局
   输入卡顿；超过 LowLevelHooksTimeout 钩子还会被系统静默摘除，CapsEnhance
   就此"莫名其妙失效"却没有任何日志。send.c 的"逐格打断"机制本质上是为
   绕开这个根因打的补丁。

   做法：主线程只管和弦引擎状态，把"真正会阻塞的发送动作"整体投递到这里
   串行执行。投递项自带开火快照，工作线程不回头读主线程的 volatile 状态，
   两边只通过队列通信。队列是 FIFO，所以"先开火、后补齐"这类顺序语义与
   原来的同步执行完全一致。

   线程起不来时（极罕见）ActPost 退回同步执行，行为等同旧版 —— 宁可损失
   响应性，也不能让功能失效。 */

#define ACT_QUEUE_MAX 64

static CRITICAL_SECTION s_cs;
static HANDLE  s_evWork = NULL;          /* auto-reset：有活干           */
static HANDLE  s_evIdle = NULL;          /* manual-reset：空闲           */
static HANDLE  s_thread = NULL;
static volatile LONG s_stop = 0;
static volatile LONG s_busy = 0;         /* 1 = 正在执行某个动作         */
static BOOL    s_init = FALSE;

static ActJob  s_q[ACT_QUEUE_MAX];
static int     s_head = 0, s_tail = 0, s_n = 0;

/* 执行一条请求。顺序必须与"开火 -> 补齐"的原始语义一致。
   走哪条分支由动作标志决定（见 keymap.c），这里不再列举动作 id ——
   加一个词跳/诊断类动作不必再回来改这个函数。 */
static void RunJob(const ActJob* j) {
    int act = j->act;
    if (act != ACT_NONE) {
        if (ActHasFlag(act, ACTF_WORDJUMP)) {
            int dir = ActWordDir(act);
            if (dir >= 0) NavJump(dir, j->shf);
        } else if (ActHasFlag(act, ACTF_PROBE)) {
            /* 焦点探测（C+`）：NavProbe 会做跨进程 SendMsg + UIA COM 往返，
               是几百毫秒级的阻塞。留在主线程就是同一个根因（见文件头）——那时
               松开 Caps 的 keyup 会被系统跳过，g_cMod 卡住，之后按 Tab 就冒出
               大写锁定。 */
            NavProbe();
        } else {
            SendActOnce(act, j->shf);
        }
    }
    if (j->fillAct != ACT_NONE) ReleaseFill(j->fillAct, j->enh, j->shf, j->count);
}

/* 队列已空且当前不在执行 -> 置空闲事件。
   s_evIdle 是 manual-reset，必须在投递时复位，否则 ActDrain 会忙转。 */
static void SignalIdleIfEmpty(void) {
    EnterCriticalSection(&s_cs);
    if (s_n == 0 && !InterlockedCompareExchange(&s_busy, 0, 0))
        SetEvent(s_evIdle);
    LeaveCriticalSection(&s_cs);
}

static DWORD WINAPI ActThread(LPVOID p) {
    (void)p;
    for (;;) {
        /* 1s 超时是保险：即便某次 SetEvent 被吞掉也不会永久挂住 */
        if (InterlockedCompareExchange(&s_stop, 0, 0)) break;
        WaitForSingleObject(s_evWork, 1000);
        for (;;) {
            ActJob j;
            EnterCriticalSection(&s_cs);
            if (s_stop) { LeaveCriticalSection(&s_cs); return 0; }
            if (s_n == 0) { LeaveCriticalSection(&s_cs); break; }
            j = s_q[s_head];
            s_head = (s_head + 1) % ACT_QUEUE_MAX;
            s_n--;
            /* 在锁内就标记"忙"：否则 ActDrain 可能看到"队列空且不忙"
               而提前返回，测试与退出流程都会踩到 */
            InterlockedExchange(&s_busy, 1);
            LeaveCriticalSection(&s_cs);

            RunJob(&j);

            InterlockedExchange(&s_busy, 0);
            SignalIdleIfEmpty();
        }
    }
    return 0;
}

/* ================= 滚轮加速专用通道 =================
   为什么不并入上面的动作队列，而要单独一条队列 + 单独一个线程：
     · 上面那条队列带 ActInFlight() 门控，滚轮挤进去会拖慢和弦重复的节奏；
     · 它是严格 FIFO，滚轮会排在"正在跑的词跳"后面（词跳可能几百毫秒），
       滚动的手感会被拖垮。
   但真正的根因是：滚轮加速原本在**主线程**上 Sleep(2ms) 逐格补发。主线程一旦
   不在"等消息"状态，系统对低级钩子的处理是**直接跳过**（不是延后补发，见
   test_chord 的 SinkWait 注释）——于是松开 Caps 的那个 keyup 可能被整个漏掉，
   g_cMod 卡在 TRUE：表现就是"松开 C 之后滚轮还在加速"，而且之后按 Tab 会被
   当成 Caps+Tab 而**冒出大写锁定**；同一根因也让 mul>=3 出现卡顿（每补一格就
   要 Sleep 2ms）。搬到本线程之后，Sleep 的只有这里，主线程始终能泵消息。 */
#define ACT_WHEEL_QMAX 64   /* 待补队列长度；满则丢最旧（宁可掉加速，也不要堆延迟） */

static CRITICAL_SECTION s_wcs;
static HANDLE  s_wEvWork = NULL;    /* auto-reset：有滚轮要补             */
static HANDLE  s_wThread = NULL;
static volatile LONG s_wStop = 0;
static BOOL    s_wInit = FALSE;
static int     s_wDelta[ACT_WHEEL_QMAX];
static int     s_wMul[ACT_WHEEL_QMAX];
static int     s_wHead = 0, s_wTail = 0, s_wN = 0;

static DWORD WINAPI WheelThread(LPVOID p) {
    (void)p;
    for (;;) {
        if (InterlockedCompareExchange(&s_wStop, 0, 0)) break;
        WaitForSingleObject(s_wEvWork, 1000);   /* 1s 超时兜底，防事件被吞 */
        for (;;) {
            int delta, mul;
            EnterCriticalSection(&s_wcs);
            if (s_wStop) { LeaveCriticalSection(&s_wcs); return 0; }
            if (s_wN == 0) { LeaveCriticalSection(&s_wcs); break; }
            delta = s_wDelta[s_wHead];
            mul   = s_wMul[s_wHead];
            s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
            s_wN--;
            LeaveCriticalSection(&s_wcs);
            SendWheelAccel(delta, mul);   /* 里面的 Sleep 只阻塞本线程 */
        }
    }
    return 0;
}

/* 投递一次滚轮加速。**主线程调用，绝不阻塞**（原来的 Sleep 就是缺陷根源）。 */
void ActWheelPost(int delta, int mul) {
    if (delta == 0 || SendWheelExtraCount(mul) <= 0) return;
    if (!s_wInit) { SendWheelAccel(delta, mul); return; }  /* 线程起不来：同步兜底 */
    EnterCriticalSection(&s_wcs);
    if (s_wStop) { LeaveCriticalSection(&s_wcs); return; }
    if (s_wN == ACT_WHEEL_QMAX) {          /* 满：丢最旧的一条 */
        s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
        s_wN--;
    }
    s_wDelta[s_wTail] = delta;
    s_wMul[s_wTail]   = mul;
    s_wTail = (s_wTail + 1) % ACT_WHEEL_QMAX;
    s_wN++;
    LeaveCriticalSection(&s_wcs);
    SetEvent(s_wEvWork);
}

/* 丢弃还没补发的滚轮请求（松开 C / 引擎复位时）。
   用户的预期是"松开 C 就是普通滚轮"，所以松手后不该再有加速尾流。 */
void ActWheelClear(void) {
    if (!s_wInit) return;
    EnterCriticalSection(&s_wcs);
    s_wHead = s_wTail = s_wN = 0;
    LeaveCriticalSection(&s_wcs);
}

/* 起/停滚轮通道。起不来不算致命：s_wInit 保持 FALSE，ActWheelPost 退回同步注入
   （损失一点响应性，但功能不失效）。 */
static void WheelLaneStart(void) {
    s_wStop = 0; s_wHead = s_wTail = s_wN = 0;
    InitializeCriticalSection(&s_wcs);
    s_wEvWork = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_wThread = s_wEvWork ? CreateThread(NULL, 0, WheelThread, NULL, 0, NULL) : NULL;
    if (!s_wEvWork || !s_wThread) {
        if (s_wThread)  { CloseHandle(s_wThread);  s_wThread = NULL; }
        if (s_wEvWork)  { CloseHandle(s_wEvWork);  s_wEvWork = NULL; }
        DeleteCriticalSection(&s_wcs);
        s_wInit = FALSE;
        return;
    }
    s_wInit = TRUE;
}
static void WheelLaneStop(void) {
    if (!s_wInit) return;
    InterlockedExchange(&s_wStop, 1);
    ActWheelClear();
    SetEvent(s_wEvWork);                  /* 唤醒可能正在等的线程 */
    if (s_wThread) {
        /* 单次补发是有界的（最多 15 格 × 2ms），给足 1s 等待即可 */
        WaitForSingleObject(s_wThread, 1000);
        CloseHandle(s_wThread); s_wThread = NULL;
    }
    if (s_wEvWork) { CloseHandle(s_wEvWork); s_wEvWork = NULL; }
    DeleteCriticalSection(&s_wcs);
    s_wInit = FALSE;
}

/* ================= 自动滚动专用通道 =================
   增强模式（默认 Alt / D）+ 滚轮 = 持续滚动（接口说明见 cap.h）。
   为什么再开一条独立通道，而不是塞进动作队列或滚轮加速队列：
     · 它是一个**长时间运行**的循环（一直滚到用户松手），而动作队列的语义是
       "一条一条执行完就走"；
     · 它不该受 ActInFlight() 门控（那门控是给和弦重复限速的）；
     · 每个节拍都要等待，和滚轮加速一样**绝不能跑在主线程上**（主线程不在
       "等消息"状态会让系统跳过它的低级钩子事件，见文件头）。

   速度模型：**每拍恒定只发一格**，只用"节拍长度"调速。
     · 第 1 档（刚启动）节拍 = AutoScrollInterval，也就是最慢档、初始速度；
     · 同向每再滚一次进一档，节拍线性插值缩短，到末档恰好缩到
       AutoScrollInterval / WheelScrollMul —— 最快档的吞吐量仍等于
       WheelScrollMul 倍，但中间还有若干档，不会"一滚就翻倍"；
     · 变速**不打断当前节拍**（下一拍才生效）。否则滚轮每动一下就立刻多插
       一格，"档位越高抖得越厉害"；
     · 反向滚 = 立即终止**并把档位清零**，之后再滚从第 1 档（最慢）重新开始。
   旧写法是"每拍补 `档位` 格"：第 2 档起就是"一小串脉冲 + 一大段静默"，
   看上去在抖 —— 这就是本模型要替换掉的东西。

   状态机（active/dir/gear）由主线程在锁内更新，工作线程只按拍输出。
   终止路径：反向滚（主线程置停）、松开修饰键（钩子投 WM_MODE_UP）、
   松开 C / 引擎复位（ResetEngine -> ActAutoScrollStop）。工作线程另外每拍
   自查 ModeDown 与 g_cMod 作兜底 —— 不依赖任何消息也能自愈。 */

/* 速度档数与节拍下限。档位 i ∈ 1..AUTOSCROLL_SPEED_STEPS：
     tick(i) = base * (S-1) / ((S-1) + (cap-1)*(i-1))
       i = 1 -> base        （初始速度，最慢）
       i = S -> base / cap  （最快 = WheelScrollMul 倍）
   线性插值把中间几档均匀铺开；档数 S 与 cap 无关，所以缺省 cap=2 时也有
   6 档可走 —— 这正是"调速不平滑"的修法（原先只有 1x / 2x 两个速度点）。 */
#define AUTOSCROLL_SPEED_STEPS 6
#define AUTOSCROLL_TICK_MIN    20   /* 节拍下限（ms）：再快没有意义，注入也太密 */

static DWORD AutoScrollTickMs(int gear, int cap) {
    int base = ConfigGetAutoScrollInterval();
    if (cap  < 2) cap  = 2;
    if (gear < 1) gear = 1;
    if (gear > AUTOSCROLL_SPEED_STEPS) gear = AUTOSCROLL_SPEED_STEPS;
    int den = (AUTOSCROLL_SPEED_STEPS - 1) + (cap - 1) * (gear - 1);
    DWORD ms = (DWORD)(base * (AUTOSCROLL_SPEED_STEPS - 1) / den);
    return (ms < AUTOSCROLL_TICK_MIN) ? AUTOSCROLL_TICK_MIN : ms;
}

static CRITICAL_SECTION s_asCs;
static HANDLE          s_asEvPoke = NULL;  /* auto-reset：状态变了（启动/变速/停止） */
static HANDLE          s_asThread = NULL;
static volatile LONG   s_asStop   = 0;
static BOOL            s_asInit   = FALSE;
static volatile LONG   s_asActive = 0;
static volatile LONG   s_asGear   = 0;     /* 速度档：1..AUTOSCROLL_SPEED_STEPS */
static volatile LONG   s_asDir    = 0;     /* +1 上滚 / -1 下滚 */
static volatile LONG   s_asCap    = 0;

int ActAutoScrollCap(void) {
    int mul = ConfigGetWheelScrollMul();
    return (mul >= 2) ? mul : 0;   /* 与滚轮加速同一个开关：0/1 = 整个滚轮增强关闭 */
}
BOOL ActAutoScrollActive(void) { return s_asActive != 0; }
int  ActAutoScrollGear(void)   { return s_asActive ? (int)s_asGear : 0; }
int  ActAutoScrollDir(void)    { return s_asActive ? (int)s_asDir  : 0; }
/* 单测用：当前节拍。变速只改节拍、不改每拍格数，所以断言它最直观。 */
int  ActAutoScrollTick(void)   {
    return s_asActive ? (int)AutoScrollTickMs((int)s_asGear, (int)s_asCap) : 0;
}

/* 立即终止。非阻塞：只置状态 + 唤醒工作线程，绝不在调用线程里等它收尾
   （调用方可能是键盘钩子所在的主线程）。 */
void ActAutoScrollStop(void) {
    if (s_asInit) EnterCriticalSection(&s_asCs);
    s_asActive = 0; s_asGear = 0; s_asDir = 0;
    if (s_asInit) { LeaveCriticalSection(&s_asCs); SetEvent(s_asEvPoke); }
}

/* 主线程调用：增强模式下的一次滚轮。非阻塞（只更新状态）。 */
void ActAutoScrollWheel(int delta) {
    if (delta == 0 || !s_asInit) return;
    int cap = ActAutoScrollCap();
    if (cap < 2) return;                       /* 未启用 */
    int dir = (delta > 0) ? 1 : -1;
    EnterCriticalSection(&s_asCs);
    s_asCap = cap;
    if (!s_asActive) {
        s_asActive = 1; s_asDir = dir; s_asGear = 1;    /* 第一滚：最慢档启动 */
    } else if ((int)s_asDir == dir) {
        if ((int)s_asGear < AUTOSCROLL_SPEED_STEPS)
            s_asGear++;                                 /* 同向：平滑进一档 */
    } else {
        /* 反向：立即终止，并**清零档位** —— 下次启动从最慢档重来，
           不残留打断前的速度（用户要的"反向打断后重置滚动速度"）。 */
        s_asActive = 0; s_asDir = 0; s_asGear = 0;
    }
    LeaveCriticalSection(&s_asCs);
    SetEvent(s_asEvPoke);
}

static DWORD WINAPI AutoScrollThread(LPVOID p) {
    (void)p;
    BOOL running = FALSE;
    for (;;) {
        if (InterlockedCompareExchange(&s_asStop, 0, 0)) break;

        if (!running) {
            /* 空闲：等一次状态变化（1s 超时兜底，防事件被吞）。
               先看状态再看事件 —— 否则"激活事件已被上一轮吃掉"时会白等 1s。 */
            if (!InterlockedCompareExchange(&s_asActive, 0, 0))
                WaitForSingleObject(s_asEvPoke, 1000);
            if (InterlockedCompareExchange(&s_asStop, 0, 0)) break;
            if (!InterlockedCompareExchange(&s_asActive, 0, 0)) continue;
            running = TRUE;          /* 落到下面立刻发第一格：启动要即时响应 */
        }

        if (!InterlockedCompareExchange(&s_asActive, 0, 0)) { running = FALSE; continue; }
        /* 兜底：增强模式或映射模式已失效（松开 Alt/D 或 C）就停 */
        if (!g_cMod || !ModeDown(MODE_ENHANCE)) {
            ActAutoScrollStop(); running = FALSE; continue;
        }
        int dir = (int)s_asDir;
        if (dir == 0) { running = FALSE; continue; }   /* 防御：不该出现 */

        DWORD tick = AutoScrollTickMs((int)s_asGear, (int)s_asCap);
        SendWheelNotches(dir > 0 ? WHEEL_STD_DELTA : -WHEEL_STD_DELTA, 1);

        /* 等满这一个节拍。**变速不打断本拍**（下一拍才生效）：未选中的唤醒
           （提速）继续等剩下的时间；只有停止/反向（active 被清 0）才提前返回。
           这就是"档位越高越抖"的修法之一。 */
        ULONGLONG deadline = GetTickCount64() + tick;
        BOOL ended = FALSE;
        for (;;) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            if (WaitForSingleObject(s_asEvPoke, (DWORD)(deadline - now)) == WAIT_TIMEOUT) break;
            if (InterlockedCompareExchange(&s_asStop, 0, 0)) { ended = TRUE; break; }
            if (!InterlockedCompareExchange(&s_asActive, 0, 0)) { ended = TRUE; break; }
        }
        if (ended) {
            if (InterlockedCompareExchange(&s_asStop, 0, 0)) break;
            running = FALSE;
        }
    }
    return 0;
}

/* 起/停自动滚动通道。起不来不算致命：s_asInit 保持 FALSE，ActAutoScrollWheel
   直接忽略 —— "持续滚动"没法像补一格那样在同一调用里同步兜底。 */
static void AutoScrollLaneStart(void) {
    s_asStop = 0;
    s_asActive = 0; s_asGear = 0; s_asDir = 0; s_asCap = 0;
    InitializeCriticalSection(&s_asCs);
    s_asEvPoke = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_asThread = s_asEvPoke ? CreateThread(NULL, 0, AutoScrollThread, NULL, 0, NULL) : NULL;
    if (!s_asEvPoke || !s_asThread) {
        if (s_asThread) { CloseHandle(s_asThread); s_asThread = NULL; }
        if (s_asEvPoke) { CloseHandle(s_asEvPoke); s_asEvPoke = NULL; }
        DeleteCriticalSection(&s_asCs);
        s_asInit = FALSE;
        return;
    }
    s_asInit = TRUE;
}
static void AutoScrollLaneStop(void) {
    if (!s_asInit) return;
    InterlockedExchange(&s_asStop, 1);
    ActAutoScrollStop();          /* 置停 + 唤醒（否则它还在节拍等待里） */
    if (s_asThread) {
        /* 每个节拍都是有界的（最多 15 格 × 2ms），给足 1s 等待即可 */
        WaitForSingleObject(s_asThread, 1000);
        CloseHandle(s_asThread); s_asThread = NULL;
    }
    if (s_asEvPoke) { CloseHandle(s_asEvPoke); s_asEvPoke = NULL; }
    DeleteCriticalSection(&s_asCs);
    s_asInit = FALSE;
}

BOOL ActStart(void) {
    if (s_init) return TRUE;
    memset(s_q, 0, sizeof(s_q));
    s_head = s_tail = s_n = 0;
    s_stop = 0; s_busy = 0;
    InitializeCriticalSection(&s_cs);
    s_evWork = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_evIdle = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!s_evWork || !s_evIdle) {
        if (s_evWork) CloseHandle(s_evWork);
        if (s_evIdle) CloseHandle(s_evIdle);
        s_evWork = s_evIdle = NULL;
        DeleteCriticalSection(&s_cs);
        return FALSE;
    }
    s_thread = CreateThread(NULL, 0, ActThread, NULL, 0, NULL);
    if (!s_thread) {
        CloseHandle(s_evWork); CloseHandle(s_evIdle);
        s_evWork = s_evIdle = NULL;
        DeleteCriticalSection(&s_cs);
        return FALSE;
    }
    s_init = TRUE;
    WheelLaneStart();      /* 滚轮加速的独立线程（起不来则退回主线程同步兜底） */
    AutoScrollLaneStart(); /* 自动滚动的独立线程（起不来则该功能静默不可用） */
    return TRUE;
}

/* 停止线程。调用后本进程不再执行任何动作（退出/重启专用）。 */
void ActStop(void) {
    if (!s_init) return;
    AutoScrollLaneStop();                /* 先停自动滚动：它可能还在持续注入 */
    WheelLaneStop();                     /* 再停滚轮通道：它只注入鼠标滚轮 */
    InterlockedExchange(&s_stop, 1);
    SendArrowsRequestInterrupt();        /* 正在逐格输出时立刻止住 */
    ActClear();
    SetEvent(s_evWork);                  /* 唤醒可能在 Wait 的线程 */
    if (s_thread) {
        /* 等当前动作收尾：所有发送都是有限步的，被打断后最坏也只是几次
           SendMsg 超时。加超时兜底，避免退出流程被卡死。
           必须等它退出 —— 之后 SendClearStuckModifiers 要读 send.c 的
           g_injectedDown，那是工作线程会写的状态。 */
        WaitForSingleObject(s_thread, 5000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    s_init = FALSE;
    if (s_evWork) { CloseHandle(s_evWork); s_evWork = NULL; }
    if (s_evIdle) { CloseHandle(s_evIdle); s_evIdle = NULL; }
    DeleteCriticalSection(&s_cs);
}

void ActPost(const ActJob* job) {
    if (!job) return;
    if (!s_init) { RunJob(job); return; }     /* 线程不可用：同步兜底 */
    EnterCriticalSection(&s_cs);
    if (s_stop) { LeaveCriticalSection(&s_cs); return; }
    if (s_n == ACT_QUEUE_MAX) {
        /* 队列满（理论上不会：引擎把飞行中的动作数限制在个位数）。
           丢最旧的一条，保证最近的动作总能执行到。 */
        s_head = (s_head + 1) % ACT_QUEUE_MAX;
        s_n--;
    }
    s_q[s_tail] = *job;
    s_tail = (s_tail + 1) % ACT_QUEUE_MAX;
    s_n++;
    ResetEvent(s_evIdle);
    LeaveCriticalSection(&s_cs);
    SetEvent(s_evWork);
}

/* 丢弃尚未开始的请求（引擎复位时用）。正在执行的那一条不受影响 ——
   它会被 SendArrowsRequestInterrupt 提前打断。 */
void ActClear(void) {
    if (!s_init) return;
    EnterCriticalSection(&s_cs);
    s_head = s_tail = s_n = 0;
    if (!InterlockedCompareExchange(&s_busy, 0, 0)) SetEvent(s_evIdle);
    LeaveCriticalSection(&s_cs);
}

/* 丢请求但保留"松手补齐"（fillAct != NONE）：
   补齐是本次点按的一部分，用户紧接着松开 C 也不能丢；
   而重复开火是映射结束后不该再产生的尾流，必须清掉。 */
void ActClearKeepFill(void) {
    if (!s_init) return;
    ActJob keep[ACT_QUEUE_MAX];
    int kn = 0;
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < s_n; i++) {
        ActJob* j = &s_q[(s_head + i) % ACT_QUEUE_MAX];
        if (j->fillAct != ACT_NONE && kn < ACT_QUEUE_MAX) keep[kn++] = *j;
    }
    s_head = 0; s_tail = 0; s_n = 0;
    for (int i = 0; i < kn; i++) {
        s_q[s_tail] = keep[i];
        s_tail = (s_tail + 1) % ACT_QUEUE_MAX;
        s_n++;
    }
    if (s_n == 0 && !InterlockedCompareExchange(&s_busy, 0, 0)) SetEvent(s_evIdle);
    LeaveCriticalSection(&s_cs);
}

/* 队列中 + 正在执行的动作数。引擎用它保持"同一时刻只有一发在飞" ——
   这正是原来同步执行时的天然行为（主线程被占住时定时器根本跑不了），
   否则长按词跳键会在队列里堆出一串尾流，松手后还在跳。 */
int ActInFlight(void) {
    if (!s_init) return 0;
    EnterCriticalSection(&s_cs);
    int n = s_n + (InterlockedCompareExchange(&s_busy, 0, 0) ? 1 : 0);
    LeaveCriticalSection(&s_cs);
    return n;
}

/* 等待队列清空且当前动作结束（测试与退出用） */
void ActDrain(void) {
    if (!s_init) return;
    for (;;) {
        EnterCriticalSection(&s_cs);
        BOOL done = (s_n == 0) && !InterlockedCompareExchange(&s_busy, 0, 0);
        LeaveCriticalSection(&s_cs);
        if (done) return;
        WaitForSingleObject(s_evIdle, 50);
    }
}
