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

/* ================= 滚轮注入通道（横向滚动用） =================
   纵向那套（Caps+滚轮 / Caps+Alt+滚轮）已经不在这里了 —— 它现在是"连续滚动"，
   走下面的连续滚动通道。本通道只剩**横向**：一次注入 n 个 ±120 的横轴刻度。
   （保留成独立通道而不是塞进连续滚动通道，是因为语义完全不同：
     横向是"一次滚轮换 n 列"的瞬时动作，不是"按某个间隔持续输出"。）

   为什么不并入上面的动作队列：那条队列带 ActInFlight() 门控、且严格 FIFO，
   会被几百毫秒的词跳拖住。
   但真正的根因是：注入里带 Sleep（`SendWheelAxis` 逐格间隔 2ms）。主线程一旦
   不在"等消息"状态，系统对低级钩子的处理是**直接跳过**（不是延后补发，见
   test_chord 的 SinkWait 注释）——于是松开 Caps 的那个 keyup 可能被整个漏掉，
   g_cMod 卡在 TRUE：表现就是"松开 C 之后滚轮还在动"，而且之后按 Tab 会被
   当成 Caps+Tab 而**冒出大写锁定**。搬到本线程之后，Sleep 的只有这里。 */
#define ACT_WHEEL_QMAX 64   /* 待补队列长度；满则丢最旧（宁可掉加速，也不要堆延迟） */

static CRITICAL_SECTION s_wcs;
static HANDLE  s_wEvWork = NULL;    /* auto-reset：有滚轮要补             */
static HANDLE  s_wThread = NULL;
static volatile LONG s_wStop = 0;
static BOOL    s_wInit = FALSE;
static int     s_wDelta[ACT_WHEEL_QMAX];  /* 方向（符号有用，幅度由注入端归一） */
static int     s_wN1[ACT_WHEEL_QMAX];     /* 注入几格 */
static int     s_wHead = 0, s_wTail = 0, s_wN = 0;

static DWORD WINAPI WheelThread(LPVOID p) {
    (void)p;
    for (;;) {
        if (InterlockedCompareExchange(&s_wStop, 0, 0)) break;
        WaitForSingleObject(s_wEvWork, 1000);   /* 1s 超时兜底，防事件被吞 */
        for (;;) {
            int delta, n;
            EnterCriticalSection(&s_wcs);
            if (s_wStop) { LeaveCriticalSection(&s_wcs); return 0; }
            if (s_wN == 0) { LeaveCriticalSection(&s_wcs); break; }
            delta = s_wDelta[s_wHead];
            n     = s_wN1[s_wHead];
            s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
            s_wN--;
            LeaveCriticalSection(&s_wcs);
            SendWheelHNotches(delta, n);   /* 里面的 Sleep 只阻塞本线程 */
        }
    }
    return 0;
}

/* 投递一次横向滚动（Caps+Shift+滚轮）。**主线程调用，绝不阻塞**。
   n = 要注入的格数（= WheelHScrollMul）。物理那一格是**纵向**事件、已被钩子
   吞掉（不吞就会横纵混滚），所以我们负责全部 n 格 —— 于是"倍数 1"恰好等于
   一次标准横向滚动。 */
void ActWheelPostH(int delta, int n) {
    if (delta == 0 || n <= 0) return;
    if (!s_wInit) { SendWheelHNotches(delta, n); return; }  /* 线程起不来：同步兜底 */
    EnterCriticalSection(&s_wcs);
    if (s_wStop) { LeaveCriticalSection(&s_wcs); return; }
    if (s_wN == ACT_WHEEL_QMAX) {          /* 满：丢最旧的一条 */
        s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
        s_wN--;
    }
    s_wDelta[s_wTail] = delta;
    s_wN1[s_wTail]    = n;
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
/* 起/停滚轮通道。起不来不算致命：s_wInit 保持 FALSE，ActWheelPostH 退回同步注入
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
        /* 单次注入是有界的（最多 15 格 × 2ms），给足 1s 等待即可 */
        WaitForSingleObject(s_wThread, 1000);
        CloseHandle(s_wThread); s_wThread = NULL;
    }
    if (s_wEvWork) { CloseHandle(s_wEvWork); s_wEvWork = NULL; }
    DeleteCriticalSection(&s_wcs);
    s_wInit = FALSE;
}

/* ================= 自动滚动专用通道 =================
   增强模式（默认 Alt / D）+ 滚轮 = 持续滚动（接口说明见 cap.h）。
   平滑注入（默认开）让注入点变密（25 次/秒），所以"绝不能跑在主线程上"
   这条约束更重要了 —— 每次注入只是一条 INPUT、没有 Sleep（SendWheelNotches
   只在 n>1 时才 Sleep，这里 n 恒为 1），但积少成多同样会占住主线程。

   为什么再开一条独立通道，而不是塞进动作队列或滚轮加速队列：
     · 它是一个**长时间运行**的循环（一直滚到用户松手），而动作队列的语义是
       "一条一条执行完就走"；
     · 它不该受 ActInFlight() 门控（那门控是给和弦重复限速的）；
     · 每个节拍都要等待，和滚轮加速一样**绝不能跑在主线程上**（主线程不在
       "等消息"状态会让系统跳过它的低级钩子事件，见文件头）。

   速度模型：速度和粒度分开 —— **速度**用"每格之间的间隔"表达，
   **粒度**（每拍挪多少）可选整格或小数刻度。

   两种档位（都走这套机制，只是起始档不同）：
     · 手动档（只有 Caps）：**直接以最快档**（WheelScrollMs）起步；
     · 增强档（Caps + 增强修饰键）：从 AutoScrollInterval 起步，逐档提速。
   速度阶梯（增强档）：
     · 第 1 档 = AutoScrollInterval（最慢、初始）；第 S 档 = WheelScrollMs（最快）；
     · 中间各档在**速度**上线性插值（等价于节拍几何插值）—— 每档的倍率相同，
       手感才均匀。最快的吞吐量恰好等于"手动滚轮"的速度，不会超过；
     · 变速**不打断当前节拍**（下一拍才生效）。否则滚轮每动一下就立刻多插
       一格，"档位越高抖得越厉害"；
     · 反向滚 = 立即终止**并把档位清零**，之后再滚从起步档重新开始。
   旧写法是"每拍补 `档位` 格"：第 2 档起就是"一小串脉冲 + 一大段静默"，
   看上去在抖 —— 这就是本模型要替换掉的东西。

   粒度（AutoScrollSmooth，默认开）：整格模式每拍发 120（原样一格）；平滑模式
   把"一格"拆成若干**小数刻度**、同时把节拍缩短到 AUTOSCROLL_SMOOTH_TICK ——
   两者速度完全等价（delta/beat 不变），但注入点更多、每次更小，于是像触控板
   那样连续滑动，而不是"一格一格跳"。Windows 文档明确支持小数 zDelta
   （"WHEEL_DELTA 的倍数或约数"），触控板正是这么滚的；目标程序若不做小数
   累积就会完全不动，那种程序把 AutoScrollSmooth 设为 0 即可。

   状态机（active/dir/gear/manual）由主线程在锁内更新，工作线程只按拍输出。
   终止路径：反向滚（主线程置停）、松开修饰键（钩子投 WM_MODE_UP，只对增强档
   生效）、松开 C / 引擎复位（ResetEngine -> ActAutoScrollStop）。工作线程另外
   每拍自查 g_cMod（以及增强档下的 ModeDown）作兜底 —— 不依赖任何消息也能自愈。 */

/* 速度档数与节拍下限。档位 i ∈ 1..AUTOSCROLL_SPEED_STEPS：
       i = 1 -> AutoScrollInterval （初始速度，最慢）
       i = S -> WheelScrollMs      （最快 = 手动滚轮的速度，不会超过）
   中间在"速度"（格/秒）上线性插值，所以每档的倍率相同、手感均匀。
   档数 S 是固定的，所以 AutoScrollInterval / WheelScrollMs 相差多少倍，
   阶梯就有多少倍可走（两者相等时阶梯退化为恒定速度）。 */
#define AUTOSCROLL_SPEED_STEPS 6
#define AUTOSCROLL_TICK_MIN    20   /* 节拍下限（ms）：再快没有意义，注入也太密 */

/* 滚轮最快速度（毫秒/格）。<= 0 表示"关闭纵向滚轮增强"（两个档位都不可用）。 */
static int AutoScrollFastMs(void) {
    int v = ConfigGetWheelScrollMs();
    return (v > 0) ? v : 0;
}

/* 平滑模式的固定节拍（ms）：25 次/秒。比它更快的注入对手感没有增益，
   只会更费 CPU（注入本身有系统开销），所以平滑模式不是"越快越好"。 */
#define AUTOSCROLL_SMOOTH_TICK 40

/* 速度档 -> "每格的时长"(ms)。这是**速度**的唯一表达：值越小越快。 */
static DWORD AutoScrollTickMs(int gear) {
    int slow = ConfigGetAutoScrollInterval();
    int fast = AutoScrollFastMs();
    if (fast <= 0)   fast = slow;   /* 未启用：阶梯退化（正常走不到这里） */
    if (fast > slow) fast = slow;   /* 配置不当（"最快"比初始还慢）：退化为恒定 */
    if (gear < 1) gear = 1;
    if (gear > AUTOSCROLL_SPEED_STEPS) gear = AUTOSCROLL_SPEED_STEPS;
    if (gear == 1) return (DWORD)slow;
    if (gear == AUTOSCROLL_SPEED_STEPS) return (DWORD)fast;
    /* 速度单位取"格/秒 × 1000"，在它上面线性插值；节拍 = 1000000 / 速度 */
    long long spSlow = 1000000LL / slow;
    long long spFast = 1000000LL / fast;
    long long sp = spSlow + (spFast - spSlow) * (gear - 1) / (AUTOSCROLL_SPEED_STEPS - 1);
    if (sp < 1) sp = 1;
    DWORD ms = (DWORD)(1000000LL / sp);
    return (ms < AUTOSCROLL_TICK_MIN) ? AUTOSCROLL_TICK_MIN : ms;
}

/* 本拍"实际怎么发"：间隔 beat(ms) + 增量 delta（无符号，方向由调用方加）。
   整格模式：beat = 每格时长，delta = WHEEL_STD_DELTA。
   平滑模式：先按 AUTOSCROLL_SMOOTH_TICK 定一个**整数** delta，再由 delta 反解
   节拍 —— 这样 delta 不会因四舍五入掉到 0，慢速档也能正常输出：
     delta = round(120 * SMOOTH_TICK / 每格时长)     （至少 1）
     beat  = round(每格时长 * delta / 120)
   于是 delta/beat 恒等于 120/每格时长（速度不变），只是拆得更碎。
   注意慢到"每格时长 > 120*SMOOTH_TICK"(=4800ms) 时 delta 已经是最小的 1，
   此时节拍自然超过 SMOOTH_TICK（注入变疏）—— 这是"整刻度的物理下限"，
   不是缺陷：一个 120 的刻度拆不出比 1 更小的整数部分。
   若某档本来就快过 SMOOTH_TICK（快速档/小 base），保持整格，免得 delta > 120。 */
static void AutoScrollPlan(int gear, DWORD* beat, int* delta) {
    DWORD gearTick = AutoScrollTickMs(gear);
    if (ConfigGetAutoScrollSmooth() && gearTick > AUTOSCROLL_SMOOTH_TICK) {
        int d = (int)((WHEEL_STD_DELTA * (LONG)AUTOSCROLL_SMOOTH_TICK + (LONG)gearTick / 2)
                      / (LONG)gearTick);          /* 四舍五入 */
        if (d < 1) d = 1;
        DWORD b = (DWORD)(((LONG)gearTick * d + WHEEL_STD_DELTA / 2) / WHEEL_STD_DELTA);
        if (b < 1) b = 1;
        *beat  = b;
        *delta = d;
    } else {
        *beat  = gearTick;
        *delta = WHEEL_STD_DELTA;
    }
}

static CRITICAL_SECTION s_asCs;
static HANDLE          s_asEvPoke = NULL;  /* auto-reset：状态变了（启动/变速/停止） */
static HANDLE          s_asThread = NULL;
static volatile LONG   s_asStop   = 0;
static BOOL            s_asInit   = FALSE;
static volatile LONG   s_asActive = 0;
static volatile LONG   s_asGear   = 0;     /* 速度档：1..AUTOSCROLL_SPEED_STEPS */
static volatile LONG   s_asDir    = 0;     /* +1 上滚 / -1 下滚 */
static volatile LONG   s_asManual = 0;     /* 手动档（只有 Caps，直接最快速度） */

/* 最快档速度（毫秒/格）；0 = 纵向滚轮增强关闭。 */
int  ActAutoScrollFastMs(void) { return AutoScrollFastMs(); }
BOOL ActAutoScrollActive(void) { return s_asActive != 0; }
BOOL ActAutoScrollManual(void) { return s_asManual != 0; }
int  ActAutoScrollGear(void)   { return s_asActive ? (int)s_asGear : 0; }
int  ActAutoScrollDir(void)    { return s_asActive ? (int)s_asDir  : 0; }
/* 单测用：当前速度（每格多少毫秒）。与粒度无关 —— 平滑与否它都一样，
   所以它是"速度"的判据；Beat/Delta 才是"本拍怎么发"的判据。 */
int  ActAutoScrollTick(void)   {
    return s_asActive ? (int)AutoScrollTickMs((int)s_asGear) : 0;
}
int  ActAutoScrollBeat(void)   {
    if (!s_asActive) return 0;
    DWORD beat; int delta;
    AutoScrollPlan((int)s_asGear, &beat, &delta);
    return (int)beat;
}
int  ActAutoScrollDelta(void)  {
    if (!s_asActive) return 0;
    DWORD beat; int delta;
    AutoScrollPlan((int)s_asGear, &beat, &delta);
    return ((int)s_asDir >= 0) ? delta : -delta;   /* 带方向 */
}

/* 立即终止。非阻塞：只置状态 + 唤醒工作线程，绝不在调用线程里等它收尾
   （调用方可能是键盘钩子所在的主线程）。 */
void ActAutoScrollStop(void) {
    if (s_asInit) EnterCriticalSection(&s_asCs);
    s_asActive = 0; s_asGear = 0; s_asDir = 0; s_asManual = 0;
    if (s_asInit) { LeaveCriticalSection(&s_asCs); SetEvent(s_asEvPoke); }
}

/* 只在"增强档"时终止。WM_MODE_UP 是"增强修饰键抬起"的通知；手动档
   （只按 Caps）本来就没按修饰键，不该被它打断。 */
void ActAutoScrollStopEnh(void) {
    if (s_asInit) EnterCriticalSection(&s_asCs);
    BOOL manual = (s_asManual != 0);
    if (!manual) { s_asActive = 0; s_asGear = 0; s_asDir = 0; }
    if (s_asInit) { LeaveCriticalSection(&s_asCs); SetEvent(s_asEvPoke); }
}

/* 主线程调用：Caps(+增强修饰) 下的一次滚轮。非阻塞（只更新状态）。
   manual = TRUE -> 手动档（只有 Caps），直接以最快档起步；
   manual = FALSE -> 增强档，从最慢档起步、同向滚逐档提速。 */
void ActAutoScrollWheel(int delta, BOOL manual) {
    if (delta == 0 || !s_asInit) return;
    if (AutoScrollFastMs() <= 0) return;       /* WheelScrollMs=0：纵向滚轮增强关闭 */
    int dir = (delta > 0) ? 1 : -1;
    int startGear = manual ? AUTOSCROLL_SPEED_STEPS : 1;
    EnterCriticalSection(&s_asCs);
    if (!s_asActive) {
        s_asActive = 1; s_asDir = dir; s_asGear = startGear; s_asManual = manual ? 1 : 0;
    } else if ((int)s_asDir != dir) {
        /* 反向：立即终止，并**清零档位** —— 下次启动从起步档重来，
           不残留打断前的速度（"反向打断后重置滚动速度"）。 */
        s_asActive = 0; s_asDir = 0; s_asGear = 0; s_asManual = 0;
    } else if ((BOOL)s_asManual != manual) {
        /* 同向但**换了档**：滚动途中按下/松开增强修饰键。
           按新档重新起步（手动档 -> 最快档；增强档 -> 最慢档），
           而不是"继续按上一档加速" —— 否则"按下 Alt 想慢一点"反而会接着加速。
           注意"松开 Alt 立即终止"是另一条路径（钩子投 WM_MODE_UP ->
           ActAutoScrollStopEnh），它先于下一次滚轮到达，所以这里不会互相打架。 */
        s_asGear = startGear; s_asManual = manual ? 1 : 0;
    } else if ((int)s_asGear < AUTOSCROLL_SPEED_STEPS) {
        s_asGear++;                                     /* 同向同档：平滑进一档 */
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
        /* 兜底：映射模式失效（松开 C）就停；增强档还要看修饰键。
           手动档只依赖 C —— 它没有修饰键可松开。 */
        if (!g_cMod || (!s_asManual && !ModeDown(MODE_ENHANCE))) {
            ActAutoScrollStop(); running = FALSE; continue;
        }
        int dir = (int)s_asDir;
        if (dir == 0) { running = FALSE; continue; }   /* 防御：不该出现 */

        DWORD tick; int delta;
        AutoScrollPlan((int)s_asGear, &tick, &delta);
        SendWheelNotches(dir > 0 ? delta : -delta, 1);

        /* 等满这一个节拍。**变速不打断本拍**（下一拍才生效）：未选中的唤醒
           （提速）继续等剩下的时间；只有停止/反向（active 被清 0）才提前返回。
           这就是"档位越高越抖"的修法之一。
           平滑模式下节拍固定为 AUTOSCROLL_SMOOTH_TICK，变速只改 delta。 */
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
    s_asActive = 0; s_asGear = 0; s_asDir = 0; s_asManual = 0;
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
