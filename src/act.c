#include "cap.h"
#include "lane.h"

#include <math.h>       /* pow()：速度阶梯的几何插值 */

/* ================= 动作执行线程 =================
   为什么需要它（这不是"优化"，是在修一个架构级缺陷）：

   WH_KEYBOARD_LL 的回调**在安装它的那个线程**上执行，而钩子由主线程安装
   （dwThreadId = 0 只表示"全局钩子"，不表示"系统线程"）。一次按词跳转在主线程
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

/* 临界区 / 事件 / 线程 / 停止位四件套交给 lane.c（见 lane.h）。本文件三条
   车道（动作队列 / 滚轮补格 / 持续滚动）+ 拖拽 + 输入法，原先五份样板逐行相同。
   s_evIdle 是**本条车道独有**的第二只事件（manual-reset：队列空且不在执行），
   服务 ActDrain 与"同一时刻只有一发在飞"的门控，所以不进 Lane。 */
static Lane    s_lane;
static HANDLE  s_evIdle = NULL;          /* manual-reset：空闲           */
static volatile LONG s_dead = 0;         /* 1 = 已停止：投递一律丢弃     */
static volatile LONG s_busy = 0;         /* 1 = 正在执行某个动作         */

/* 统一的 Interlocked 读：s_dead 与各车道的 active/gear/dir 这类跨线程标量
   一律走 Interlocked —— 混用会被 MSVC /analyze 判为 C28112，而且确实没有
   理由混用。车道自己的 stop 位归 lane.c 独占，本文件不再直接读写它。 */
#define STOPPED(p) (InterlockedCompareExchange((p), 0, 0) != 0)
#define ACTIVE(p)  (InterlockedCompareExchange((p), 0, 0) != 0)
#define ATOMIC(p)  (InterlockedCompareExchange((p), 0, 0))

static ActJob  s_q[ACT_QUEUE_MAX];
static int     s_head = 0, s_tail = 0, s_n = 0;

/* ---------- Hyper 意图信箱（HyperPassthrough，语义见 send.c） ----------
   钩子线程只写一个 int（1=按下 / -1=抬起 / 0=无），动作线程每轮循环开头
   消费一次。为什么不排队列：Hyper 的按下/抬起必须与"正在发键的突发"串行
   —— 列在 FIFO 里会排到长按词跳转后面（几百毫秒才生效），直接插队又可能
   落进某个突发的中间。信箱语义恰好折中：**只在两次动作之间**执行，突发
   进行中最多等一个突发（几十毫秒），突发结束后立即生效。
   collapse 是安全的：信箱只表达"最终想要的状态"，重复 apply 幂等。
   程序退出（ActStop）时未消费的意图被丢弃 —— 按下的四键由
   SendClearStuckModifiers 兜底抬起（都在注入记账 g_injMask 里）。 */
static std::atomic<int> s_hyperWish{ 0 };

void ActHyperSet(BOOL on) {
    s_hyperWish.store(on ? 1 : -1);
    LanePoke(&s_lane);          /* 线程可能在空闲等待，戳一下 */
}

/* 执行一条请求。顺序必须与"开火 -> 补齐"的原始语义一致。
   走哪条分支由动作标志决定（见 keymap.c），这里不再列举动作 id ——
   加一个按词跳转/诊断类动作不必再回来改这个函数。 */
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
        } else if (ActHasFlag(act, ACTF_MOVEWIN)) {
            /* 挪窗口（C+Q）：SetWindowPos 要等对方窗口过程处理完位置变化，
               对方卡住就能把调用者拖住 —— 和执行线程存在的理由同源（见文件头），
               所以也在这一层执行，不走主线程。 */
            if (!WinMoveFocusedToCursor() && g_recording.load())
                CapLog("  [窗口] 没有可挪的前台窗口（桌面/任务栏，或就是本程序自己）");
        } else if (ActHasFlag(act, ACTF_OPENFOLDER)) {
            /* 打开所在文件夹（C+\）：目标窗口取的是 ActJob 里的**开火快照**
               （见 cap.h 的 ActJob.hwnd），不是现在现读前台窗口。
               这里只投递：真正的 shell 调用在 folder.c 的一次性线程上跑，
               所以这条分支**不会**把后面的动作堵在队列里（文件头那条"队列是
               FIFO"的顾虑对它不成立）。 */
            FolderOpenRequest(j->hwnd);
        } else {
            /* 诊断：按键动作"发了"与"目标程序认了"是两件事 —— 用户看到
               "按了没反应"时，只有回读光标才能区分。只在记录模式下读，
               且读的是同一个焦点控件（不做任何缓存/状态改动）。 */
            BOOL dbg = g_recording.load();
            long long c0 = dbg ? NavFocusCaret() : -1;
            SendActOnce(act, j->shf);
            if (dbg) {
                long long c1 = NavFocusCaret();
                CapLog("  [注入] %s -> vk=0x%02X shift=%d  光标 %lld -> %lld%s",
                       ActName(act), ActOutVk(act), j->shf ? 1 : 0, c0, c1,
                       (c0 >= 0 && c1 == c0) ? "   <<< 发了但光标没动" : "");
            }
        }
    }
    if (j->fillAct != ACT_NONE) ReleaseFill(j->fillAct, j->enh, j->shf, j->count);
}

/* 队列已空且当前不在执行 -> 置空闲事件。
   s_evIdle 是 manual-reset，必须在投递时复位，否则 ActDrain 会忙转。 */
static void SignalIdleIfEmpty(void) {
    EnterCriticalSection(&s_lane.cs);
    if (s_n == 0 && !InterlockedCompareExchange(&s_busy, 0, 0))
        SetEvent(s_evIdle);
    LeaveCriticalSection(&s_lane.cs);
}

static DWORD WINAPI ActThread(LPVOID p) {
    (void)p;
    for (;;) {
        /* 1s 超时是保险：即便某次 SetEvent 被吞掉也不会永久挂住 */
        if (LaneStopped(&s_lane)) break;
        WaitForSingleObject(s_lane.ev, 1000);
        for (;;) {
            /* Hyper 意图在每轮动作之间消化：突发进行中不插手（见信箱注释） */
            int wish = s_hyperWish.exchange(0);
            if (wish != 0) SendHyperApply(wish > 0);
            ActJob j;
            EnterCriticalSection(&s_lane.cs);
            if (LaneStopped(&s_lane)) { LeaveCriticalSection(&s_lane.cs); return 0; }
            if (s_n == 0) { LeaveCriticalSection(&s_lane.cs); break; }
            j = s_q[s_head];
            s_head = (s_head + 1) % ACT_QUEUE_MAX;
            s_n--;
            /* 在锁内就标记"忙"：否则 ActDrain 可能看到"队列空且不忙"
               而提前返回，测试与退出流程都会踩到 */
            InterlockedExchange(&s_busy, 1);
            LeaveCriticalSection(&s_lane.cs);

            /* 执行耗时打点：排查"按了键却要等一会儿才动"时，一眼看出是
               本次动作本身慢（NavJump 的 IPC/COM），还是被前一条堵住了
               （wait = 从投递到开始执行）。只在记录模式下写。 */
            ULONGLONG tWait0 = GetTickCount64();
            if (g_recording.load() && (j.act != ACT_NONE || j.fillAct != ACT_NONE))
                CapLog("  [执行] 开始 act=%s fill=%s 排队等待=%llu ms (队列剩 %d)",
                       ActName(j.act), ActName(j.fillAct),
                       (unsigned long long)(tWait0 - j.postTick), s_n);
            RunJob(&j);
            if (g_recording.load() && (j.act != ACT_NONE || j.fillAct != ACT_NONE))
                CapLog("  [执行] 完成 act=%s 耗时=%llu ms", ActName(j.act),
                       (unsigned long long)(GetTickCount64() - tWait0));

            InterlockedExchange(&s_busy, 0);
            SignalIdleIfEmpty();
        }
    }
    return 0;
}

/* ================= 滚轮注入通道（纵向放大 / 横向滚动） =================
   两种"瞬时"滚轮动作共用这一条通道（`Caps+Alt/D+滚轮` 的持续滚动走下面
   另一条通道，因为它是"按某个间隔持续输出"，语义不同）：
     · 纵向放大（`Caps+滚轮`）：倍数 N -> 补发 (N-1) 格（物理那格系统照常送达）；
     · 横向滚动（`Caps+Shift+滚轮`）：物理那格是纵向事件、已被钩子吞掉，
       所以我们注入**全部** N 格（于是"倍数 1" = 一次标准横向滚动）。
   两者只差 dwFlags 与 n 的算法，共用一条队列 + 线程。

   为什么不并入上面的动作队列：那条队列带 ActInFlight() 门控、且严格 FIFO，
   会被几百毫秒的按词跳转拖住。
   但真正的根因是：注入里带 Sleep（`SendWheelAxis` 逐格间隔 2ms）。主线程一旦
   不在"等消息"状态，系统对低级钩子的处理是**直接跳过**（不是延后补发，见
   test_chord 的 SinkWait 注释）——于是松开 Caps 的那个 keyup 可能被整个漏掉，
   g_cMod 卡在 TRUE：表现就是"松开 C 之后滚轮还在动"，而且之后按 Tab 会被
   当成 Caps+Tab 而**冒出大写锁定**。搬到本线程之后，Sleep 的只有这里。 */
#define ACT_WHEEL_QMAX 64   /* 待补队列长度；满则丢最旧（宁可掉加速，也不要堆延迟） */
/* 这条请求已经等了多久才算"注入追不上滚轮" -> 跳过格间间隔（send.c 的 hurry）。
   定在 20ms：正常跟手时一条只要 ~9ms（3 格 × 1.2ms + 2 个 2ms 间隔），
   等到 20ms 说明前面至少压着两条，再讲究"逐格不合并"就是拿掉加速去换精度。
   松手判据 ACT_WHEEL_STALE_MS 在 cap_window.h（main.c 的 ResetEngine 也要用）。 */
#define ACT_WHEEL_HURRY_MS  20

static Lane    s_wLane;            /* 车道四件套（见 lane.h）            */
static int     s_wDelta[ACT_WHEEL_QMAX];  /* 方向（符号有意义，幅度由注入端归一） */
static int     s_wMul[ACT_WHEEL_QMAX];    /* 纵向：放大倍数；横向：注入格数 */
static BOOL    s_wHorz[ACT_WHEEL_QMAX];   /* 本条是横向吗（Caps+Shift+滚轮） */
static BOOL    s_wFull[ACT_WHEEL_QMAX];   /* 纵向全量注入（Hyper 下物理格也被吞，
                                              mul 格全部由我们补发，见 WheelPost） */
static ULONGLONG s_wTick[ACT_WHEEL_QMAX]; /* 入队时刻（GetTickCount64）：诊断用 */
static int     s_wHead = 0, s_wTail = 0, s_wN = 0;

static DWORD WINAPI WheelThread(LPVOID p) {
    (void)p;
    for (;;) {
        if (LaneStopped(&s_wLane)) break;
        WaitForSingleObject(s_wLane.ev, 1000);   /* 1s 超时兜底，防事件被吞 */
        for (;;) {
            int delta, mul; BOOL horz;
            EnterCriticalSection(&s_wLane.cs);
            if (LaneStopped(&s_wLane)) { LeaveCriticalSection(&s_wLane.cs); return 0; }
            if (s_wN == 0) { LeaveCriticalSection(&s_wLane.cs); break; }
            delta = s_wDelta[s_wHead];
            mul   = s_wMul[s_wHead];
            horz  = s_wHorz[s_wHead];
            BOOL full = s_wFull[s_wHead];
            ULONGLONG tIn = s_wTick[s_wHead];   /* 入队时刻：算"排队等了多久" */
            s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
            s_wN--;
            int depth = s_wN;                   /* 出队后还剩几条：>0 = 这条通道在积压 */
            LeaveCriticalSection(&s_wLane.cs);
            /* 里面的等待只阻塞本线程 */
            int notches = horz ? mul : (full ? mul : SendWheelExtraCount(mul));
            ULONGLONG t0 = GetTickCount64();
            ULONGLONG waited = t0 - tIn;
            /* 积压判据用"这条已经等了多久"，而不是"队列深度"：深度 1 只是正常
               跟手，等到 ACT_WHEEL_HURRY_MS 才说明注入真的追不上滚轮。 */
            BOOL hurry = (waited >= ACT_WHEEL_HURRY_MS);
            if (horz)      SendWheelHNotchesEx(delta, mul, hurry);   /* 横向：注入 mul 格 */
            else if (full) SendWheelNotchesEx(delta, mul, hurry);    /* 纵向全量：物理格已被吞 */
            else           SendWheelNotchesEx(delta, SendWheelExtraCount(mul), hurry);
                                                        /* 纵向：物理格已送达，补 mul-1 */
            /* 记录模式：这一行是"终点侧"的证据 —— 钩子那行（hook.c 的"滚轮 纵 …"）
               只说"系统送来了什么"，本行说"我们真的补了几格、排队等了多久"。
               排查"有时候不加速"时两种异常一眼可辨：
                 · 「排队」突然变成几十上百毫秒 = 注入被目标拖住、队列在积压，
                   后面的请求就会被 WheelPost 的"满则丢最旧"或松手清队丢掉；
                 · 「补=0 格」= 倍数被配成 <=1（该组合已关闭），不是丢包。 */
            if (g_recording.load())
                CapLog("  滚轮注入 %s delta=%d 补=%d 格 排队=%llu ms 注入=%llu ms 队列剩=%d%s",
                       horz ? "横" : "纵", delta, notches,
                       (unsigned long long)waited,
                       (unsigned long long)(GetTickCount64() - t0), depth,
                       hurry ? "  [积压:已跳过格间隔]" : "");
        }
    }
    return 0;
}

/* 投递一次滚轮请求。**主线程调用，绝不阻塞**（原来的 Sleep 就是缺陷根源）。
   horz=FALSE：纵向放大，n = 倍数（只补 n-1 格，物理那格系统照常送达）；
   horz=TRUE ：横向滚动，n = 注入格数（物理那格是纵向、已被钩子吞掉，
               所以全部由我们注入）。
   full：纵向**全量**注入（n 格全由我们发）—— Hyper 透传下物理那格也必须
   吞掉（它带着按住的 RShift 到达目标会被当成横向/缩放手势），改由我们摘掉
   Hyper 后整量注入。 */
static void WheelPost(int delta, int n, BOOL horz, BOOL full) {
    if (delta == 0) return;
    if (n <= 0) return;
    if (!horz && !full && SendWheelExtraCount(n) <= 0) {   /* 倍数 <2：不放大 */
        /* 记一行：否则日志里只有钩子那句"滚轮 纵 …"，看不出"为什么后面什么都没有"
           —— 排查"有时候不加速"时，"是配置关了"和"是被丢了"必须能分开。 */
        if (g_recording.load())
            CapLog("  滚轮补发：跳过（倍数<=1，该组合已关闭）n=%d", n);
        return;
    }
    if (!LaneLive(&s_wLane)) {
        /* 分两种，不能一概"同步兜底"：
             · 线程**从未**起来（CreateThread 失败）-> 同步注入，宁可损失
               响应性也不能让功能失效；
             · 已经 ActStop() 停掉了 -> **直接丢弃**。退出/重启路径是
               ActStop() -> ... -> HookUninstall()，钩子在 ActStop 之后仍然
               装着，这段窗口期内再来滚轮事件就会在**主线程**同步跑
               SendWheelAxis 的 Sleep —— 正是本文件头要消灭的根因。 */
        if (STOPPED(&s_dead)) {
            if (g_recording.load())
                CapLog("  滚轮补发：丢弃（通道已停止）delta=%d n=%d", delta, n);
            return;
        }
        if (g_recording.load())
            CapLog("  滚轮补发：通道未起，退回主线程同步注入 delta=%d n=%d", delta, n);
        if (horz) SendWheelHNotches(delta, n);
        else if (full) SendWheelNotches(delta, n);
        else      SendWheelAccel(delta, n);
        return;
    }
    EnterCriticalSection(&s_wLane.cs);
    if (LaneStopped(&s_wLane)) {
        LeaveCriticalSection(&s_wLane.cs);
        if (g_recording.load())
            CapLog("  滚轮补发：丢弃（车道已停）delta=%d n=%d", delta, n);
        return;
    }
    BOOL dropped = FALSE;
    if (s_wN == ACT_WHEEL_QMAX) {          /* 满：丢最旧的一条 */
        s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
        s_wN--;
        dropped = TRUE;
    }
    s_wDelta[s_wTail] = delta;
    s_wMul[s_wTail]   = n;
    s_wHorz[s_wTail]  = horz;
    s_wFull[s_wTail]  = full;
    s_wTick[s_wTail]  = GetTickCount64();
    s_wTail = (s_wTail + 1) % ACT_WHEEL_QMAX;
    s_wN++;
    int depth = s_wN;
    LeaveCriticalSection(&s_wLane.cs);
    /* 日志写在锁**外**：CapLog 是文件 I/O，占着队列锁会把注入线程一起堵住 ——
       那正好是"主线程/钩子被阻塞"的同一种味道（见文件头）。 */
    if (g_recording.load())
        CapLog("  滚轮补发：入队 %s delta=%d n=%d 队列=%d/%d%s",
               horz ? "横" : "纵", delta, n, depth, ACT_WHEEL_QMAX,
               dropped ? "（队列满，丢了最旧一条）" : "");
    LanePoke(&s_wLane);
}

void ActWheelPost(int delta, int mul)  { WheelPost(delta, mul, FALSE, SendHyperLive()); }
void ActWheelPostH(int delta, int n)   { WheelPost(delta, n,   TRUE,  FALSE);          }

/* 丢弃**全部**还没补发的滚轮请求（通道停止 / 退出 / 暂停用）。
   ★ 松开 C 不走这里了 —— 见下面的 ActWheelDropOlder：整队清空会把用户刚滚的
   那几格一起吞掉，那正是"有时候不加速、松开重按才行"的成因。 */
void ActWheelClear(void) {
    if (!LaneLive(&s_wLane)) return;
    EnterCriticalSection(&s_wLane.cs);
    int dropped = s_wN;
    s_wHead = s_wTail = s_wN = 0;
    LeaveCriticalSection(&s_wLane.cs);
    /* 丢了多少必须写下来：否则"钩子看到 68 格、只注入 42 次"这种缺口在日志里
       一点痕迹都没有（这次排查就卡在这上面）。 */
    if (g_recording.load() && dropped > 0)
        CapLog("  滚轮补发：清空队列，丢弃 %d 条未补发请求", dropped);
}

/* 队头是否"陈旧"。抽成纯函数是为了能被单测直接断言 —— 时间戳是内部状态，
   不伪造时钟就测不到这条边界，而它正是"松手会不会吞掉用户刚滚的那几格"
   的唯一判据。ms<=0 表示整队清空。 */
static BOOL WheelStale(ULONGLONG enqTick, ULONGLONG now, int ms) {
    if (ms <= 0) return TRUE;
    return (now - enqTick) > (ULONGLONG)ms;
}

/* 只丢"陈旧"的请求（松开 C / 引擎复位时用）。队列是 FIFO，所以从队头开始，
   超过 ms 的才丢，新的留着继续补发。ms<=0 等同于整队清空（单测用）。
   为什么不再整队清空：那些请求就是用户刚刚滚出来的加速，清掉等于让这次甩轮
   的尾部"不加速"；而重按一次 C 队列变空、下一轮又正常。 */
void ActWheelDropOlder(int ms) {
    if (!LaneLive(&s_wLane)) return;
    int dropped = 0;
    EnterCriticalSection(&s_wLane.cs);
    ULONGLONG now = GetTickCount64();
    while (s_wN > 0 && WheelStale(s_wTick[s_wHead], now, ms)) {
        s_wHead = (s_wHead + 1) % ACT_WHEEL_QMAX;
        s_wN--;
        dropped++;
    }
    int left = s_wN;
    LeaveCriticalSection(&s_wLane.cs);
    if (g_recording.load() && dropped > 0)
        CapLog("  滚轮补发：松手清掉 %d 条陈旧请求（>%d ms），保留 %d 条继续补发",
               dropped, ms, left);
}
/* 起/停滚轮通道。起不来不算致命：车道没起来（LaneLive 为假），两个投递入口都退回同步注入
   （损失一点响应性，但功能不失效）。 */
static void WheelLaneStart(void) {
    s_wHead = s_wTail = s_wN = 0;
    memset(s_wHorz, 0, sizeof(s_wHorz));
    memset(s_wFull, 0, sizeof(s_wFull));
    memset(s_wTick, 0, sizeof(s_wTick));
    LaneStart(&s_wLane, WheelThread);
}
static void WheelLaneStop(void) {
    LaneStopRequest(&s_wLane);
    ActWheelClear();                      /* 松手后不该再有加速尾流 */
    /* 单次注入是有界的（最多 15 格 × 2ms），给足 1s 等待即可 */
    LaneJoin(&s_wLane, 1000);
}

/* ================= 持续滚动专用通道 =================
   只有 `Caps + 增强修饰键（默认 Alt/D） + 滚轮` 走这里（接口说明见 cap.h）：
   滚一下就自己一直滚，与滚轮是否还在动无关。
   平滑注入（默认开）让注入点变密（25 次/秒），所以"绝不能跑在主线程上"
   这条约束更重要了 —— 每次注入只是一条 INPUT、没有 Sleep（SendWheelNotches
   只在 n>1 时才 Sleep，这里 n 恒为 1），但积少成多同样会占住主线程。

   为什么再开一条独立通道，而不是塞进动作队列或滚轮注入通道：
     · 它是一个**长时间运行**的循环（一直滚到用户松手），而动作队列的
       语义是"一条一条执行完就走"；
     · 它不该受 ActInFlight() 门控（那门控是给和弦重复限速的）；
     · 每个节拍都要等待，和滚轮加速一样**绝不能跑在主线程上**（主线程不在
       "等消息"状态会让系统跳过它的低级钩子事件，见文件头）。

   速度模型：速度和粒度分开 —— **速度**用"每格之间的间隔"表达，
   **粒度**（每拍挪多少）可选整格或小数刻度。

   速度阶梯（唯一的一种模式）：
     · 第 1 档 = AutoScrollInterval（最慢、初始）；第 S 档 = AutoScrollFastMs（最快）；
     · 中间各档在**对数**上均匀插值（几何插值）—— 每档倍率相同，手感才均匀。
       不用线性插值的原因见 AutoScrollTickMs 的注释（两端差一个数量级以上时，
       线性插值的第一步就是几十倍，"再滚一下直接飞出去"）；
     · 两端是**两个独立配置项**（一个管起步、一个管顶档），本通道完全不看
       WheelScrollMul —— 倍数量与时间量是两种量，不混用；
     · 变速**不打断当前节拍**（下一拍才生效）。否则滚轮每动一下就立刻多插
       一格，"档位越高抖得越厉害"；
     · 反向滚 = 立即终止**并把档位清零**，之后再滚从第 1 档重新开始。
   旧写法是"每拍补 `档位` 格"：第 2 档起就是"一小串脉冲 + 一大段静默"，
   看上去在抖 —— 这就是本模型要替换掉的东西。

   粒度（默认平滑）：平滑模式把"一格"拆成若干**小数刻度**、同时把节拍缩短到
   AUTOSCROLL_SMOOTH_TICK —— 两者速度完全等价（delta/beat 不变），但注入点
   更多、每次更小，于是像触控板那样连续滑动，而不是"一格一格跳"。Windows
   文档明确支持小数 zDelta（"WHEEL_DELTA 的倍数或约数"），触控板正是这么滚的。
   目标程序若不做小数累积就会完全不动 —— 那种程序写进 **[NoSmoothScroll]**
   名单即可（整格模式：每拍发 120）。

   为什么名单而不是一个全局开关：这是"个别程序的行为"，让所有程序一起陪着
   退化没有道理；而且用户往往要滚的正是"不累积小数刻度"的那个程序，全局关
   掉等于把最常用的那条路也堵了。判定在**启动那一刻**做一次（见
   ActAutoScrollSetNoSmooth）：查前台进程名要 OpenProcess，不能每拍都做。

   状态机（active/dir/gear）由主线程在锁内更新，工作线程只按拍输出。

   **终止路径（2026-09-11 改版：松手不再打断）**：
     · 反向滚（主线程置停，并清零档位）；
     · **用户产生任何新输入** —— 新按下的键（**系统自动重复不算**）、按下鼠标键、
       或**没按着和弦**的滚轮（`ActAutoScrollInterrupt`，由钩子线程调用）；
     · **超时**：从最后一次调速起算 AUTOSCROLL_MAX_MS（10 分钟），到点自行终止；
     · 暂停映射 / 退出 / 重启（`ActAutoScrollStop` / ActStop）。
   松开 Caps 或增强修饰键 **不再**终止 —— 设计意图是"滚起来就能松手读，
   想停的时候按一下键"。工作线程也不再自查 g_cMod/ModeDown（那是旧规则）。

   记录模式下每一次终止都会留一行（谁停的 + 细节 + 已注入拍数，见
   ActAutoScrollInterrupt）。本功能的终止条件全在钩子侧，症状出现时现场什么都不剩 ——
   要查它就先按 `C + \`` 开始记录：那一行与记录模式的滚轮现场就是现场。 */

/* 速度档数与节拍下限。档位 i ∈ 1..AUTOSCROLL_SPEED_STEPS：
       i = 1 -> AutoScrollInterval （初始速度，最慢）
       i = S -> AutoScrollFastMs   （最快；独立配置项）
   中间按**几何**插值（在速度的对数上均匀），所以每档倍率相同、手感均匀。
   档数 S 固定：两端相差多少倍，阶梯就有多少倍可走；两端相等则退化为恒定。
   档数只影响"每档迈多大"：两端倍数差 < S 时，每档的倍率就接近 1（很细）；
   差得越远，每档迈得越大。6 档在"滚一两次就该有明显变化"与"别太跳"之间
   比较平衡 —— 多滚几次才能到顶，长按提速的层次感也还在。 */
#define AUTOSCROLL_SPEED_STEPS 6
#define AUTOSCROLL_TICK_MIN    20   /* 节拍下限（ms）：再快没有意义，注入也太密 */

/* 持续滚动的最长持续时间：从"最后一次调速"起算，到点自动停。
   为什么要它：松手之后滚动由它自己驱动，一旦用户离开（或者钩子被系统摘掉、
   没人再送输入进来），就没有任何东西能让它停了 —— 那是"永远在滚"的最坏情况。
   10 分钟足够读完很长的一页，又不会让人回来发现还在滚。
   不是"从启动算死的硬上限"：用户在调速说明人还在，于是每次滚轮都续期。 */
#define AUTOSCROLL_MAX_MS 600000
static volatile LONG s_asMaxMs = AUTOSCROLL_MAX_MS;   /* 单测可缩短，见 cap.h */

/* 持续滚动的最快速度（毫秒/格），来自 AutoScrollFastMs。 */
static int AutoScrollFastMs(void) {
    return CfgInt(CFG_AUTOSCROLL_FAST_MS);
}

/* 平滑模式的固定节拍（ms）：25 次/秒。比它更快的注入对手感没有增益，
   只会更费 CPU（注入本身有系统开销），所以平滑模式不是"越快越好"。 */
#define AUTOSCROLL_SMOOTH_TICK 40

/* 速度档 -> "每格的时长"(ms)。这是**速度**的唯一表达：值越小越快。

   几何插值：tick(i) = slow * (fast/slow)^((i-1)/(S-1))。
   为什么不用"线性插值"（在格/秒 上等距）：两端常差一个数量级以上 ——
   缺省 1500 与 60 差 25 倍，若两端再拉开（比如把 Interval 调到 24000）就更多。
   线性插值会把第 1→2 档做成几十倍，手感上就是"再滚一下直接飞出去"，
   而后面几档几乎分不出来。几何插值让每档的**倍率**相同，过渡才均匀。 */
static DWORD AutoScrollTickMs(int gear) {
    int slow = CfgInt(CFG_AUTOSCROLL_INTERVAL);
    int fast = AutoScrollFastMs();
    if (fast > slow) fast = slow;   /* 配置不当（"最快"比初始还慢）：退化为恒定速度 */
    if (gear < 1) gear = 1;
    if (gear > AUTOSCROLL_SPEED_STEPS) gear = AUTOSCROLL_SPEED_STEPS;
    if (gear == 1) return (DWORD)slow;
    if (gear == AUTOSCROLL_SPEED_STEPS) return (DWORD)fast;
    double r  = pow((double)fast / (double)slow, 1.0 / (AUTOSCROLL_SPEED_STEPS - 1));
    double ms = (double)slow * pow(r, gear - 1);
    long   v  = (long)(ms + 0.5);
    if (v > slow) v = slow;         /* 两端严格钳住，避免浮点越界 */
    if (v < fast) v = fast;
    if (v < AUTOSCROLL_TICK_MIN) v = AUTOSCROLL_TICK_MIN;
    return (DWORD)v;
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
   若某档本来就快过 SMOOTH_TICK（快速档/小 base），保持整格，免得 delta > 120。

   noSmooth = 前台程序命中 ** [NoSmoothScroll] ** 名单：整格模式。
   声明在**这里**（函数上方）是因为 AutoScrollPlan 既被工作线程每拍调用、
   也被单测的 ActAutoScrollBeat/Delta 调用，而"设置它"的 ActAutoScrollWheel
   在文件更下方 —— 读方在前，就必须在读方之前声明。 */
static volatile LONG   s_asNoSmooth = 0;
static void AutoScrollPlan(int gear, DWORD* beat, int* delta) {
    DWORD gearTick = AutoScrollTickMs(gear);
    if (!ATOMIC(&s_asNoSmooth) && gearTick > AUTOSCROLL_SMOOTH_TICK) {
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

static Lane            s_asLane;          /* 车道四件套（见 lane.h） */
/* 本通道的状态只有两类访问者：① **主线程**（消息处理、钩子回调、暂停/退出）；
   ② 本通道自己的工作线程（只读 + 超时那一处调 ActAutoScrollStop）。
   低级键盘/鼠标钩子的回调**跑在装钩子的线程**上（见 hook.c），也就是主线程 ——
   所以"钩子回调"与"消息处理"天然串行，不存在两者同时进临界区的可能；
   工作线程与 s_asLane.cs 的关系则被 AutoScrollLaneStop 的 LaneJoin 隔开（它在
   join 之后才删临界区）。**这两条不变量就是这里不需要"生命周期锁"的原因**：
   所有跨线程可见的东西要么是 volatile/Interlocked 标量，要么由 s_asLane.cs 保护。
   哪天要把本通道的某个入口搬到第三个线程上，先回来重读这段。 */
static volatile LONG   s_asActive = 0;
static volatile LONG   s_asGear   = 0;     /* 速度档：1..AUTOSCROLL_SPEED_STEPS */
static volatile LONG   s_asDir    = 0;     /* +1 上滚 / -1 下滚 */
/* 超时的时刻。**故意只存 32 位**：它要在两个线程之间无锁读写，32 位对齐的
   读写本身是原子的，64 位值则可能被撕裂（x86 上真要出事）。
   取的是 GetTickCount64 的**低 32 位**（而不是 GetTickCount），比较写成
   `(LONG)(deadline - now) <= 0` —— 回绕语义下只要真实差值 < 2^31 就恒正确，
   我们的上限是 10 分钟，差得远。（/analyze 会提示 GetTickCount 每 49 天回绕，
   这里正是按那条提示做的：既避开它，又保留单字宽的无锁读写。） */
static volatile LONG   s_asDeadline = 0;
/* 本次会话已经注入过多少拍。**只服务日志**：终止那行带上它，就能一眼分开
   "一拍都没发出去就被停"与"发了一拍就停"（"滚一下就停"正是后者），
   也能反过来证明"它其实一直在注入，只是目标程序没反应"。 */
static volatile LONG   s_asTicks = 0;

/* 最快档速度（毫秒/格）。 */
int  ActAutoScrollFastMs(void) { return AutoScrollFastMs(); }
BOOL ActAutoScrollActive(void) { return ACTIVE(&s_asActive); }
int  ActAutoScrollGear(void)   { return ACTIVE(&s_asActive) ? (int)ATOMIC(&s_asGear) : 0; }
int  ActAutoScrollDir(void)    { return ACTIVE(&s_asActive) ? (int)ATOMIC(&s_asDir)  : 0; }
/* 单测用：当前速度（每格多少毫秒）。与粒度无关 —— 平滑与否它都一样，
   所以它是"速度"的判据；Beat/Delta 才是"本拍怎么发"的判据。 */
int  ActAutoScrollTick(void)   {
    return ACTIVE(&s_asActive) ? (int)AutoScrollTickMs((int)ATOMIC(&s_asGear)) : 0;
}
int  ActAutoScrollBeat(void)   {
    if (!ACTIVE(&s_asActive)) return 0;
    DWORD beat; int delta;
    AutoScrollPlan((int)ATOMIC(&s_asGear), &beat, &delta);
    return (int)beat;
}
int  ActAutoScrollDelta(void)  {
    if (!ACTIVE(&s_asActive)) return 0;
    DWORD beat; int delta;
    AutoScrollPlan((int)ATOMIC(&s_asGear), &beat, &delta);
    return ((int)ATOMIC(&s_asDir) >= 0) ? delta : -delta;   /* 带方向 */
}

/* 立即终止。非阻塞：只置状态 + 唤醒工作线程，绝不在调用线程里等它收尾
   （调用方可能是键盘钩子所在的主线程）。 */
void ActAutoScrollStop(void) {
    if (LaneLive(&s_asLane)) EnterCriticalSection(&s_asLane.cs);
    InterlockedExchange(&s_asActive, 0);
    InterlockedExchange(&s_asGear, 0);
    InterlockedExchange(&s_asDir, 0);
    if (LaneLive(&s_asLane)) { LeaveCriticalSection(&s_asLane.cs); LanePoke(&s_asLane); }
}

/* 用户产生了新输入（按键 / 鼠标动作）-> 打断持续滚动。
   **钩子线程（= 主线程）会调它**，所以"没在滚"时必须几乎零成本：先读一次
   active，只有真在滚才去拿锁 + SetEvent + 写一行日志。否则等于给每一次按键/点击
   都加一次临界区与内核调用 —— 那正是本项目最忌讳的"钩子路径上干重活"。

   why / detail 只服务那行日志，**与其它诊断日志一样只在 `C + \`` 记录模式下写**
   （本项目的约定：日志文件只在记录期间生成 —— 见 README"日志与诊断"）。
   排查本功能时请先按 `C + \`` 开始记录再复现：终止条件全在钩子侧，症状出现时
   现场什么都不剩，那两行（终止原因 + 记录模式下的滚轮现场）就是现场。
   why 是调用方给的**字面量**（本函数不负责释放；也不额外做格式化）。 */
void ActAutoScrollInterrupt(const char* why, int detail) {
    if (!ACTIVE(&s_asActive)) return;
    if (g_recording.load())
        CapLog("持续滚动：终止（%s detail=%d/0x%X，已注入 %d 拍）",
               why ? why : "新输入", detail, (unsigned)(detail & 0xFFFF),
               (int)ATOMIC(&s_asTicks));
    ActAutoScrollStop();
}

/* 设/读"本次只发整格"。生产路径由 ActAutoScrollWheel 在启动那一刻按前台程序
   调用；对外暴露是为了单测能直接验证整格与小数刻度两条路（不必伪造前台窗口）。 */
void ActAutoScrollSetNoSmooth(BOOL on) {
    InterlockedExchange(&s_asNoSmooth, on ? 1 : 0);
}
BOOL ActAutoScrollNoSmooth(void) { return ATOMIC(&s_asNoSmooth) != 0; }

/* 单测用：给个机会把超时缩短（否则测一次要真等 10 分钟）。<=0 回缺省。 */
void ActAutoScrollTestSetMax(int ms) {
    InterlockedExchange(&s_asMaxMs, (ms > 0) ? (LONG)ms : (LONG)AUTOSCROLL_MAX_MS);
}

/* 单测用：距离超时还剩多少毫秒（未启动返回 0）。 */
int ActAutoScrollRemainMs(void) {
    if (!ACTIVE(&s_asActive)) return 0;
    LONG r = (LONG)(s_asDeadline - (LONG)(DWORD)GetTickCount64());
    return (r > 0) ? (int)r : 0;
}

/* 主线程调用：增强模式（Caps + 增强修饰键）下的一次滚轮。非阻塞（只更新状态）。
   第一次滚 -> 从最慢档启动；同向再滚 -> 逐档提速；反向 -> 终止并清零。
   每次调用都**续期**超时：用户在调速，说明人还在。 */
void ActAutoScrollWheel(int delta) {
    if (delta == 0 || !LaneLive(&s_asLane)) return;
    int dir = (delta > 0) ? 1 : -1;
    EnterCriticalSection(&s_asLane.cs);
    /* **先续期超时，再动 active**：工作线程是"看到 active 就去看超时"的，
       顺序反过来的话，它有可能在新 deadline 写下去之前就抢到那一拍 ——
       读到的是上一次启动留下的陈旧时刻（更早），于是刚启动就自己停掉。
       窗口很小（两条 Interlocked 之间），但"偶发一次启动即停"最难查。 */
    InterlockedExchange(&s_asDeadline, (LONG)((DWORD)GetTickCount64() + (DWORD)ATOMIC(&s_asMaxMs)));
    if (!ACTIVE(&s_asActive)) {
        /* **顺序**：方向与档位先写，最后才置 active。工作线程是"看到 active 就用
           dir/gear"，反过来写会让它读到 active=1、dir=0 的中间态 —— 那是"刚启动
           就自己停"与"空转"两种症状的来源。置 active 之前对工作线程而言都只是
           "还没在滚"，所以这样写没有任何窗口。 */
        /* 粒度只在这里定一次：查前台进程名要 OpenProcess + 查询映像名，放在
           每拍里做就是给"每一格滚动"加一次系统调用 —— 滚动期间换窗口这种
           场景不值得付这个价（换窗口后重新滚一次即会重判）。 */
        InterlockedExchange(&s_asNoSmooth,
                            ConfigIsNoSmoothScroll(GetForegroundWindow()) ? 1 : 0);
        InterlockedExchange(&s_asDir, dir);
        InterlockedExchange(&s_asGear, 1);   /* 第一滚：最慢档启动 */
        InterlockedExchange(&s_asTicks, 0);  /* 新会话：拍数从头数（日志用） */
        InterlockedExchange(&s_asActive, 1);
    } else if ((int)ATOMIC(&s_asDir) != dir) {
        /* 反向：立即终止，并**清零档位** —— 下次启动从第 1 档重来，
           不残留打断前的速度（"反向打断后重置滚动速度"）。
           记录模式下留一行（含两边方向与这一格的增量）：反向终止也是"无故停了"
           的常见来源（滚轮回弹/编码器抖动会发一个极小反向量），有日志才分得清。 */
        if (g_recording.load())
            CapLog("持续滚动：终止（反向滚 %+d -> %+d，delta=%d，已注入 %d 拍）",
                   (int)ATOMIC(&s_asDir), dir, delta, (int)ATOMIC(&s_asTicks));
        InterlockedExchange(&s_asDir, 0);
        InterlockedExchange(&s_asGear, 0);
        InterlockedExchange(&s_asActive, 0);
    } else if ((int)ATOMIC(&s_asGear) < AUTOSCROLL_SPEED_STEPS) {
        InterlockedExchange(&s_asGear, (LONG)ATOMIC(&s_asGear) + 1);  /* 同向：平滑进一档 */
    }
    LeaveCriticalSection(&s_asLane.cs);
    LanePoke(&s_asLane);
}

static DWORD WINAPI AutoScrollThread(LPVOID p) {
    (void)p;
    BOOL running = FALSE;
    for (;;) {
        if (LaneStopped(&s_asLane)) break;

        if (!running) {
            /* 空闲：等一次状态变化（1s 超时兜底，防事件被吞）。
               先看状态再看事件 —— 否则"激活事件已被上一轮吃掉"时会白等 1s。 */
            if (!ACTIVE(&s_asActive))
                WaitForSingleObject(s_asLane.ev, 1000);
            if (LaneStopped(&s_asLane)) break;
            if (!ACTIVE(&s_asActive)) continue;
            running = TRUE;          /* 落到下面立刻发第一格：启动要即时响应 */
        }

        if (!ACTIVE(&s_asActive)) { running = FALSE; continue; }

        /* 超时兜底：从最后一次调速起算 AUTOSCROLL_MAX_MS。到点自己停 ——
           这一条不依赖任何输入，是"用户走开 / 钩子被系统摘掉"时的最后一道闸。 */
        LONG remain = (LONG)(s_asDeadline - (LONG)(DWORD)GetTickCount64());
        if (remain <= 0) {
            if (g_recording.load())
                CapLog("持续滚动：终止（超时 %d ms 未调速，已注入 %d 拍）",
                       (int)ATOMIC(&s_asMaxMs), (int)ATOMIC(&s_asTicks));
            ActAutoScrollStop(); running = FALSE; continue;
        }
        int dir = (int)ATOMIC(&s_asDir);
        if (dir == 0) {
            /* 防御：正常不会出现（dir 在 active 之前写入，见 ActAutoScrollWheel）。
               真出现说明状态被打乱 —— **必须停下**，不能只把 running 清掉了事：
               active 还是 1，下一轮会跳过"空闲等待"直接回来，于是这里变成没有
               任何等待的死循环（CPU 空转、界面照旧，极难查）。 */
            ActAutoScrollStop(); running = FALSE; continue;
        }

        DWORD tick; int delta;
        AutoScrollPlan((int)ATOMIC(&s_asGear), &tick, &delta);
        SendWheelNotches(dir > 0 ? delta : -delta, 1);
        InterlockedIncrement(&s_asTicks);   /* 只服务日志（见 s_asTicks 声明） */

        /* 等满这一个节拍。**变速不打断本拍**（下一拍才生效）：未选中的唤醒
           （提速）继续等剩下的时间；只有停止/反向（active 被清 0）才提前返回。
           这就是"档位越高越抖"的修法之一。
           平滑模式下节拍固定为 AUTOSCROLL_SMOOTH_TICK，变速只改 delta。
           等待上限同时被"剩余超时"钳住：最慢档一拍 60s，否则超时会被拖过去。 */
        DWORD wait = tick;
        if ((DWORD)remain < wait) wait = (DWORD)remain;
        ULONGLONG deadline = GetTickCount64() + wait;
        BOOL ended = FALSE;
        for (;;) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            if (WaitForSingleObject(s_asLane.ev, (DWORD)(deadline - now)) == WAIT_TIMEOUT) break;
            if (LaneStopped(&s_asLane)) { ended = TRUE; break; }
            if (!ACTIVE(&s_asActive)) { ended = TRUE; break; }
        }
        if (ended) {
            if (LaneStopped(&s_asLane)) break;
            running = FALSE;
        }
    }
    return 0;
}

/* 起/停持续滚动通道。起不来不算致命：车道没起来（LaneLive 为假），ActAutoScrollWheel
   直接忽略 —— "持续滚动"没法像补一格那样在同一调用里同步兜底。 */
static void AutoScrollLaneStart(void) {
    InterlockedExchange(&s_asActive, 0);
    InterlockedExchange(&s_asGear, 0);
    InterlockedExchange(&s_asDir, 0);
    InterlockedExchange(&s_asDeadline, 0);
    LaneStart(&s_asLane, AutoScrollThread);
}
static void AutoScrollLaneStop(void) {
    LaneStopRequest(&s_asLane);
    ActAutoScrollStop();          /* 置停 + 唤醒（否则它还在节拍等待里） */
    /* 每个节拍都是有界的（最多 15 格 × 2ms），给足 1s 等待即可 */
    LaneJoin(&s_asLane, 1000);
}

BOOL ActStart(void) {
    /* 上一次 ActStop 等到超时（线程还在用旧的锁与事件）时，lane.c 会把该车道
       标记成 zombie 并**拒绝**重起 —— 对仍被持有的锁重做 InitializeCriticalSection
       是未定义行为（见 lane.h 不变量）。这里返回 FALSE = 调用方按"线程起不来"
       处理（退回同步兜底），与既有语义一致。 */
    if (LaneLive(&s_lane)) return TRUE;
    memset(s_q, 0, sizeof(s_q));
    s_head = s_tail = s_n = 0;
    InterlockedExchange(&s_dead, 0);
    InterlockedExchange(&s_busy, 0);
    s_evIdle = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!s_evIdle) return FALSE;
    if (!LaneStart(&s_lane, ActThread)) {
        CloseHandle(s_evIdle);
        s_evIdle = NULL;
        return FALSE;
    }
    WheelLaneStart();      /* 滚轮加速的独立线程（起不来则退回主线程同步兜底） */
    AutoScrollLaneStart(); /* 自动滚动的独立线程（起不来则该功能静默不可用） */
    DragLaneStart();       /* 拖拽窗口的独立线程（起不来则该功能静默不可用） */
    ImeLaneStart();        /* 单键切换输入法的独立线程（起不来则静默不可用） */
    return TRUE;
}

/* 停止线程。调用后本进程不再执行任何动作（退出/重启专用）。
   **注意调用方（main.c）的收尾顺序是 ActStop() -> HookUninstall()** ——
   两者之间钩子仍然装着、事件仍会进来，所以本函数**一进来就置 s_dead**：
   让所有投递入口直接丢弃请求，而不是退回"同步兜底"
   （那会把 NavJump 的数百毫秒 IPC 或滚轮注入的 Sleep 重新塞回承载钩子的
   主线程）。 */
void ActStop(void) {
    if (!LaneLive(&s_lane)) return;
    InterlockedExchange(&s_dead, 1);
    AutoScrollLaneStop();                /* 先停自动滚动：它可能还在持续注入 */
    WheelLaneStop();                     /* 再停滚轮通道：它只注入鼠标滚轮 */
    DragLaneStop();                      /* 再停拖拽通道：它只挪窗口 */
    ImeLaneStop();                       /* 再停输入法通道：它只向 IME 窗口发消息 */
    LaneStopRequest(&s_lane);
    SendArrowsRequestInterrupt();        /* 正在逐格输出时立刻止住 */
    ActClear();
    LanePoke(&s_lane);                   /* 唤醒可能在 Wait 的线程 */
    /* 等当前动作收尾：所有发送都是有限步的，被打断后最坏也只是几次
       SendMsg 超时。加超时兜底，避免退出流程被卡死。
       必须等它退出 —— 之后 SendClearStuckModifiers 要读 send.c 的
       g_injMask（注入记账），那是工作线程会写的状态。
       ★ 超时（线程还活着，可能卡在某个跨进程调用里）时由 lane.c 保留临界区与
       两只事件、把车道标成 zombie 并拒绝重起：**绝不能拆它仍在用的共享对象**
       —— 删掉的临界区会被它再次 EnterCriticalSection（未定义），关掉的事件句柄
       可能被系统复用给别的对象而它照样 SetEvent 过去 = 搅动别人的信令。
       进程马上就退，留着不关是代价最小的选择。（退出流程不因此挂住：本函数
       返回后主线程照常往下走。） */
    if (!LaneJoin(&s_lane, 5000)) {
        if (g_recording.load())
            CapLog("  !! 动作线程 5s 未退出，跳过共享对象销毁（防拆正在使用的锁/事件）");
        return;
    }
    if (s_evIdle) { CloseHandle(s_evIdle); s_evIdle = NULL; }
}

void ActPost(const ActJob* job) {
    if (!job) return;
    if (!LaneLive(&s_lane)) {
        /* 与 WheelPost 同一条规则：从未起来 -> 同步兜底；已 ActStop -> 丢弃。
           这里的同步兜底更危险 —— RunJob 可能是 NavJump（数百毫秒的跨进程
           IPC），而 ActStop() 之后钩子还没卸（见 ActStop 的注释）。 */
        if (STOPPED(&s_dead)) return;
        RunJob(job);
        return;
    }
    EnterCriticalSection(&s_lane.cs);
    if (LaneStopped(&s_lane)) { LeaveCriticalSection(&s_lane.cs); return; }
    if (s_n == ACT_QUEUE_MAX) {
        /* 队列满（理论上不会：引擎把飞行中的动作数限制在个位数）。
           丢最旧的一条，保证最近的动作总能执行到。 */
        s_head = (s_head + 1) % ACT_QUEUE_MAX;
        s_n--;
    }
    s_q[s_tail] = *job;
    s_q[s_tail].postTick = GetTickCount64();   /* 投递时刻：诊断"排队等了多久" */
    s_tail = (s_tail + 1) % ACT_QUEUE_MAX;
    s_n++;
    ResetEvent(s_evIdle);
    LeaveCriticalSection(&s_lane.cs);
    LanePoke(&s_lane);
}

/* 丢弃尚未开始的请求（引擎复位时用）。正在执行的那一条不受影响 ——
   它会被 SendArrowsRequestInterrupt 提前打断。 */
void ActClear(void) {
    if (!LaneLive(&s_lane)) return;
    EnterCriticalSection(&s_lane.cs);
    s_head = s_tail = s_n = 0;
    if (!InterlockedCompareExchange(&s_busy, 0, 0)) SetEvent(s_evIdle);
    LeaveCriticalSection(&s_lane.cs);
}

/* 丢请求但保留"本次按键本身"（keep 为真的那些）：
   用户已经按下并按着的动作（开火）与它的松手补齐，都是本次点按的一部分，
   不能因为紧接着松开 C 就被清掉；而**重复开火**是映射结束后不该再产生的
   尾流，必须丢。
   为什么要显式标 keep 而不是"看 fillAct 是否为空"：按下即开火之后，一次点按
   变成两条请求（按下开火 fillAct=空、抬起补齐 act=空），按住 fillAct 判据会把
   开火那条当尾流丢掉 —— 用户按了键却没有输出，实测就是这么丢的。 */
void ActClearKeepFill(void) {
    if (!LaneLive(&s_lane)) return;
    ActJob keep[ACT_QUEUE_MAX];
    int kn = 0;
    EnterCriticalSection(&s_lane.cs);
    for (int i = 0; i < s_n; i++) {
        ActJob* j = &s_q[(s_head + i) % ACT_QUEUE_MAX];
        if (j->keep && kn < ACT_QUEUE_MAX) keep[kn++] = *j;
    }
    s_head = 0; s_tail = 0; s_n = 0;
    for (int i = 0; i < kn; i++) {
        s_q[s_tail] = keep[i];
        s_tail = (s_tail + 1) % ACT_QUEUE_MAX;
        s_n++;
    }
    if (s_n == 0 && !InterlockedCompareExchange(&s_busy, 0, 0)) SetEvent(s_evIdle);
    LeaveCriticalSection(&s_lane.cs);
}

/* 队列中 + 正在执行的动作数。引擎用它保持"同一时刻只有一发在飞" ——
   这正是原来同步执行时的天然行为（主线程被占住时定时器根本跑不了），
   否则长按按词跳转键会在队列里堆出一串尾流，松手后还在跳。 */
int ActInFlight(void) {
    if (!LaneLive(&s_lane)) return 0;
    EnterCriticalSection(&s_lane.cs);
    int n = s_n + (InterlockedCompareExchange(&s_busy, 0, 0) ? 1 : 0);
    LeaveCriticalSection(&s_lane.cs);
    return n;
}

/* 等待队列清空且当前动作结束（测试与退出用） */
void ActDrain(void) {
    if (!LaneLive(&s_lane)) return;
    for (;;) {
        EnterCriticalSection(&s_lane.cs);
        BOOL done = (s_n == 0) && !InterlockedCompareExchange(&s_busy, 0, 0);
        LeaveCriticalSection(&s_lane.cs);
        if (done) return;
        WaitForSingleObject(s_evIdle, 50);
    }
}
