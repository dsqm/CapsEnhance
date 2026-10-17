#include "cap.h"

#include <atomic>

/* ================= 模拟按键发送 =================
   这一层只关心"把键正确地送出去"，不含任何业务逻辑。三条硬规则都是踩过坑的：

   1) 扩展键必须带 KEYEVENTF_EXTENDEDKEY。方向键/Home/End/Del 等的扫描码
      与小键盘共用，缺了扩展标志，目标程序拿到的是"小键盘 4/6"而不是
      "左/右箭头"。实测（Chromium）：不加标志时 keydown 的 code=Numpad4 /
      Numpad6，依赖 code 派发的程序（VSCode）就会认错键；加上后 code=ArrowLeft。
      反过来 VK_RETURN 绝不能加 —— 主键盘回车不扩展，加了会变成小键盘回车。

   2) 逐条 SendInput + 微小间隔，而不是一次塞一整批。批量发送在部分程序里
      只认第一个键（这也是 SendArrows 一直逐条发的原因）。

   3) 组合键里修饰键必须真的按住：Shift/Ctrl 全程在目标键之前按下、之后抬起。
      之前兜底按词跳转把 Ctrl↓ Ctrl↑ 连续发出，目标收到的方向键 ctrl=0，
      于是退化成"单格方向键"——这正是 caps+N/O 在多个程序里"只挪一格"的原因。
*/

/* 需要 KEYEVENTF_EXTENDEDKEY 的键 */
static BOOL IsExtendedVk(int vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE:
    case VK_SNAPSHOT:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_DIVIDE:                 /* 小键盘 / */
    case VK_RCONTROL: case VK_RMENU:
        /* 右 Ctrl / 右 Alt 同为 E0 前缀键（E0 1D / E0 38）：不带扩展标志，
           注入事件的合成扫描码就是裸 1D/38 —— 与左 Ctrl 和小键盘句号撞车，
           按扫描码认键的消费者（AHK 的 >^/>! 区分、日志工具）会认错边。
           右 Shift 的扫描码（36）独占，**不**扩展 —— 保持。 */
    case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        /* 多媒体键在真实键盘上同样是 E0 前缀扫描码（Mute=E0 20 / Down=E0 2E /
           Up=E0 30）。缺了扩展标志，注入事件的合成扫描码就是裸 0x20 —— 那是
           字母 D 的主键码，按扫描码认键的消费者（日志工具 / 其他钩子）会看成
           "按了 D"。 */
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---------- 注入记账：本进程注入过"按下"还没配"抬起"的所有键 ----------
   进程死在注入对的中间（崩溃 / 任务管理器 / 车道 join 超时兜底）时，这些键
   会留在"逻辑按下"状态 —— 目标程序认为它们一直按着，正是"卡键"。退出前
   无条件补发抬起（与用户是否物理按住无关：这些键本来就只该由我们按下）。

   为什么记**全部键**而不是只记修饰键：按词跳转的 Ctrl+方向、逐格补齐的
   方向键同样是注入的 down/up 对，任何一对被截断，残留的就是方向键 ——
   只记修饰键的年代它们既无账可查，也游离在清理范围之外（修"重启清不掉
   卡键"时一并改掉，见 SendClearStuckModifiers）。

   跨线程：写者是 act.c **工作线程**调用的 SendEvent；读者是退出 / 重启时
   主线程的 SendClearStuckModifiers。与被替换掉的 std::atomic<bool>[8] 同一
   级别的保证：Interlocked 原生操作让位图**自身**线程安全，不把正确性寄托
   在调用顺序上（ActStop 先 join 的顺序仍要保持，只是不再兼任"同步手段"）。 */
static volatile LONG g_injMask[8];   /* bit = vk & 31，字 = vk >> 5，覆盖 VK 0..255 */

static void InjNote(int vk, BOOL up) {
    if (vk <= 0 || vk > 255) return;
    volatile LONG* w = &g_injMask[vk >> 5];
    LONG bit = (LONG)1 << (vk & 31);
    if (up) InterlockedAnd(w, ~bit);
    else    InterlockedOr(w, bit);
}

/* 单条键盘事件（按下 / 抬起） */
static void SendEvent(int vk, BOOL up) {
    InjNote(vk, up);
    INPUT i;
    memset(&i, 0, sizeof(i));
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = (WORD)vk;
    i.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (IsExtendedVk(vk)) i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up)                i.ki.dwFlags |= KEYEVENTF_KEYUP;
    /* 返回值必须看：目标窗口的完整性级别高于本进程时（例如对方以管理员
       运行而本程序没有），UIPI 会把整批注入拒掉、SendInput 返回 0。此前
       丢弃返回值，于是现象是"按词跳转完全没反应"而日志干净无事 —— 无法诊断。
       只在记录模式写日志：常规模式不打扰用户，也不改变任何行为。 */
    if (SendInput(1, &i, sizeof(INPUT)) == 0 && g_recording.load())
        CapLog("  !! SendInput 被拒 vk=0x%02X up=%d err=%lu",
               (unsigned)vk, up ? 1 : 0, (unsigned long)GetLastError());
}

/* 事件之间的间隔：让目标程序的消息队列逐个消化 */
#define KEY_GAP        5      /* ms：同一组合键内主键的按下/抬起之间 */
#define ARROW_GAP      8      /* ms：连续箭头之间 */
/* 修饰键（Shift/Ctrl）与主键之间的间隔。实测：注入式按键在虚拟机上约
   60ms 才到达目标，间隔太小时目标处理主键时读到的修饰键状态还没更新——
   表现是"Ctrl+方向"退化成单格移动（Quicker/WPF 上抓到的：同一段文本里
   Ctrl+← 时灵时不灵，日志交替出现 -1 与整词位移）。取 40ms（本机注入
   延迟实测约 60ms/键，间隔要留足）：代价是组合键单次多 ~80ms，换来的是
   不再退化成单格移动。 */
#define COMBO_GAP      40     /* ms：修饰键 <-> 主键之间 */

/* Hyper 透传的突发摘除（定义在文件末尾）：本程序自己发键/滚轮之前先摘掉
   按住的 RWin+RCtrl+RShift+RAlt —— 否则注入的 ← 带着 RWin 出去就是 Win+←，
   直接把目标窗口贴边。返回本次是否真摘了（Resume 据此决定要不要按回去）。 */
BOOL SendHyperSuspend(void);
void SendHyperResume(BOOL was);

/* 单击一个键；shift 全程按住（Shift↓ 键↓ 键↑ Shift↑） */
void SendKeyOnce(int vk, BOOL shift) {
    BOOL hyperHeld = SendHyperSuspend();
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(COMBO_GAP); }
    SendEvent(vk, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, TRUE);
    if (shift) { Sleep(COMBO_GAP); SendEvent(VK_LSHIFT, TRUE); }
    SendHyperResume(hyperHeld);
}

/* 逐格输出的打断机制：
   循环跑在 **act.c 的动作工作线程**上（NavJump 由 RunJob 调用），不是主线程，
   所以"松开 C"和"改道按键"这些打断意图由**主线程**（钩子回调；退出时还有
   ActStop）置 volatile 标志，循环里每发完一对键检查一次（最坏 ~13ms 内响应）。
   触发条件：
     1) C（CapsLock）松开 —— 映射模式结束，立即停（hook.c）；
     2) 按下与当前输出方向不同的触发键 —— 改道（如正在 ← 时按 L）（hook.c）。
   打断后循环立即退出，循环外的收尾（抬起 Shift）照常执行；
   改道按键的 TRIG_DOWN 已在主线程队列里，循环结束后自然开火衔接。
   ★ 打断点**只能落在一对键之外**（抬起之后）。原先在按下与抬起之间也判一次，
     落在 KEY_GAP 那 5ms 里的打断就把方向键留在"逻辑按下"状态出了循环 ——
     而 SendClearStuckModifiers 只补修饰键，补不回方向键：目标程序从此认为
     VK_LEFT 一直按着，光标自己往前跑。

   ★ g_arrBusy / g_arrDir / g_arrBreak 是本项目**第三组真正跨线程**的标志
     （前两个工作线程写、主线程钩子读；g_arrBreak 主线程写、工作线程读，
     见 cap.h 的"atomic / volatile 的分界"）。它们是"意图"标志，最坏只是晚
     一拍响应，所以维持 volatile；若要改 atomic，SendArrowsConflicts 里的
     `!g_arrBusy` 必须一并改成显式 .load()。 */
static volatile BOOL g_arrBusy = FALSE;    /* 正在逐格输出 */
static volatile int  g_arrDir  = -1;       /* 当前输出方向 (DIR_*) */
static volatile BOOL g_arrBreak = FALSE;   /* 打断请求 */
/* 上一次逐格输出是否"被打断结束"。补正阶段读它：打断意味着和弦已经松手，
   那时候再发箭头就是在用户松手之后继续注键（见 SendArrowsWasInterrupted）。
   只在工作线程读写（发完即读），不跨线程。 */
static volatile BOOL g_arrBroke = FALSE;

void SendArrowsRequestInterrupt(void) { g_arrBreak = TRUE; }

/* 本次（刚刚那次）逐格输出是否被打断。仅在 SendArrows / SendArrowsStrict
   返回后、同一工作线程内读取，用来决定还要不要做回读补正。 */
BOOL SendArrowsWasInterrupted(void) { return g_arrBroke; }

/* act 是否与正在逐格输出的方向冲突（不同触发键即改道） */
BOOL SendArrowsConflicts(int act) {
    if (!g_arrBusy) return FALSE;
    int curAct = (g_arrDir == DIR_LEFT) ? ACT_WORD_L : ACT_WORD_R;
    return act != curAct;
}

/* 逐方向键移动：发送 n 个左/右箭头（可带 Shift 位移）。
   每个箭头单独发送并微小间隔，避免大批量 SendInput 在部分程序中只认第一个键；
   Shift 在整个按词跳转期间持续按住以维持选区。 */
#define MAX_ARROW_STEPS 256
/* 真正的逐格发送：不做任何超限判定，n 步就是 n 步。 */
static void ArrowsEmit(int dir, int n, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    BOOL hyperHeld = SendHyperSuspend();   /* Hyper 透传时先摘四键，逐格完再按回 */
    g_arrDir = dir; g_arrBusy = TRUE; g_arrBreak = FALSE; g_arrBroke = FALSE;
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(COMBO_GAP); }
    for (int i = 0; i < n && i < MAX_ARROW_STEPS; i++) {
        if (g_arrBreak) { g_arrBroke = TRUE; break; }
        SendEvent(vk, FALSE);
        Sleep(KEY_GAP);
        SendEvent(vk, TRUE);           /* 抬起无条件发完：见上面"打断点只能落在
                                          一对键之外"的说明 */
        if (g_arrBreak) { g_arrBroke = TRUE; break; }
        Sleep(ARROW_GAP);
    }
    g_arrBusy = FALSE;
    if (shift) SendEvent(VK_LSHIFT, TRUE);   /* 打断时也要抬 Shift，选区停在此处 */
    SendHyperResume(hyperHeld);
}

/* 补正专用：即使步数达到 MaxArrowSteps 也逐格走完，**不**改道。
   补正的步数是"目标 - 实测落点"算出来的，它不是本次要跳的距离；若再判一次
   超限就又会改道一次，而改道之后没有任何人再回读落点 —— 补正白做、光标又
   落回段中间（这正是 QQ 场景里"按词跳转落到段中间"的根因之一）。 */
void SendArrowsStrict(int dir, int n, BOOL shift) {
    ArrowsEmit(dir, n, shift);
}

int SendArrows(int dir, int n, BOOL shift) {
    /* 本次调用的"有没有被打断"从零开始算：改道那条分支不进 ArrowsEmit，
       不清就会拿上一发的结果来决定本次要不要补正。 */
    g_arrBroke = FALSE;
    /* 发送处统一判定：本次要发的步数达到 MaxArrowSteps 就改为
         OverStepMode=1 -> 发原生 Ctrl+方向（目标程序自己的按词跳转，瞬时）
         OverStepMode=0 -> 只发一个方向键（跳一格即停）
       放在这里而不是调用方，是为了让所有调用路径都受同一约束；
       MaxArrowSteps=0 表示不限制（硬上限仍是 256）。 */
    int maxSteps = CfgInt(CFG_MAX_ARROW_STEPS);
    if (maxSteps > 0 && n >= maxSteps) {
        int want = n;
        if (CfgBool(CFG_OVER_STEP_CTRL)) {
            SendNativeWordJump(dir, shift);
            if (g_recording.load())
                CapLog("  -> 逐格步数 %d 达到 MaxArrowSteps(%d)，改发 Ctrl+arrow",
                       want, maxSteps);
            /* 改道只是"让目标程序自己跳一格词"，落点由**它的**词切分决定，
               跟我们的算法不一定一致（中文尤其明显，见 cap.h 的说明），
               所以必须把"本次是改道"告诉调用方去回读校验。 */
            return ARROW_SEND_HANDOFF;
        }
        if (g_recording.load())
            CapLog("  -> 逐格步数 %d 达到 MaxArrowSteps(%d)，只发一格",
                   want, maxSteps);
        ArrowsEmit(dir, 1, shift);
        return ARROW_SEND_ONE;   /* 既定行为"跳一格即停"：不做补正 */
    }
    ArrowsEmit(dir, n, shift);
    return ARROW_SEND_STEPS;
}

/* 逐格注入 n 个"同值刻度"，纵轴与横轴共用这一段 —— 只有 dwFlags 不同
   （MOUSEEVENTF_WHEEL / MOUSEEVENTF_HWHEEL），"发几格"由调用方决定，
   注入语义完全一样，所以只留一处实现，免得"改了间隔忘了改另一条路"。

   **不要**改成"一个事件塞 delta = 120*N"，实测（跨进程量目标窗口收到的消息）：
     · 一次 SendInput 批 3 个 120    -> 目标只收到 1 条、delta=360
     · 3 个事件间隔 2ms 逐个发       -> 目标收到 3 条、每条 delta=120
   系统会把"目标还没派发的"滚轮事件合并成一个 WM_MOUSEWHEEL 并把 delta 相加；
   更糟的是 +120 与 -120 同批会被合成 delta=0，等于什么都没滚。
   为什么在乎这个差别：不少程序只看 delta 的符号、每条消息只滚一格（Blender
   就为此专门把大 delta 拆成多个单格事件），所以"逐格"才是通用写法。

   每格 2ms 的间隔在倍数上限 16 下最坏阻塞 30ms。也正因为有 Sleep，这段**不能**
   跑在主线程上：调用它的通道都在 act.c 的专用线程里（见 act.c 顶部说明）。
   全程只做输入注入：不调 SystemParametersInfo（不改系统的"每次滚动行数"），
   也不写注册表。 */
#define WHEEL_EXTRA_MAX 15
#define WHEEL_GAP_MS    2

/* ---------- 精确短等待（只给滚轮逐格间隔用） ----------
   为什么不能用 Sleep：进程没提高定时器精度时，Sleep 的最小粒度是**一个系统
   tick**（默认 15.6ms）。实测本机 Sleep(2) 单次就是 15.6ms —— 于是"补 3 格"
   要 30.5ms 而不是设计的 4ms，注入线程比滚轮送格还慢，队列一路积压（实测
   排队涨到 203ms），尾部再被"松手清队"整段丢掉，表现就是"有时候不加速"。

   为什么不用 timeBeginPeriod(1)：它改的是**全系统**的定时器精度（耗电、
   唤醒频率），一个常驻工具不该做这件事。高精度可等待定时器只影响本进程；
   老系统上建不出来就退回 Sleep（行为与旧版一致）。 */
static HANDLE    s_hiTimer = NULL;
static INIT_ONCE s_hiOnce  = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK HiTimerInit(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    s_hiTimer = CreateWaitableTimerExW(NULL, NULL,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return TRUE;
}
static void WaitMs(int ms) {
    if (ms <= 0) return;
    InitOnceExecuteOnce(&s_hiOnce, HiTimerInit, NULL, NULL);
    if (s_hiTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ms * 10000;   /* 相对时间，单位 100ns */
        if (SetWaitableTimer(s_hiTimer, &due, 0, NULL, NULL, FALSE) &&
            WaitForSingleObject(s_hiTimer, (DWORD)ms + 50) == WAIT_OBJECT_0)
            return;
    }
    Sleep((DWORD)ms);                            /* 兜底：与旧版行为一致 */
}

/* hurry：跳过格间间隔。只有**队列已经积压**时才用（判据在 act.c 的
   WheelThread）——那时宁可让目标把几格合并成一条大 delta，也不能让队列
   越堆越深、最后整段被清掉。正常跟手滚动不会走到这里，所以"逐格不合并"
   这个刻意设计（见上）不受影响。 */
static void SendWheelAxis(int delta, int n, BOOL horizontal, BOOL hurry) {
    if (delta == 0 || n <= 0) return;
    if (n > WHEEL_EXTRA_MAX) n = WHEEL_EXTRA_MAX;
    /* Hyper 透传时同样摘四键：RShift 按着时目标的 WM_MOUSEWHEEL 会被当成
       横向/缩放手势（浏览器 Shift+滚轮 = 横向），纵轴放大就走样了。 */
    BOOL hyperHeld = SendHyperSuspend();
    for (int i = 0; i < n; i++) {
        INPUT in;
        memset(&in, 0, sizeof(in));
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)(LONG)delta;   /* 有符号增量：正=上/右，负=下/左 */
        if (SendInput(1, &in, sizeof(INPUT)) == 0) {
            /* UIPI 等场景会被整体拒绝。只在记录模式写日志、且不重试：
               重试只会继续失败并把日志刷爆 */
            if (g_recording.load())
                CapLog("  !! SendInput 滚轮被拒 %s i=%d delta=%d err=%lu",
                       horizontal ? "横" : "纵", i, delta,
                       (unsigned long)GetLastError());
            break;
        }
        if (i + 1 < n && !hurry) WaitMs(WHEEL_GAP_MS);
    }
    SendHyperResume(hyperHeld);
}

void SendWheelNotches(int delta, int n)  { SendWheelAxis(delta, n, FALSE, FALSE); }
void SendWheelNotchesEx(int delta, int n, BOOL hurry) {
    SendWheelAxis(delta, n, FALSE, hurry);
}

/* 纵向放大：倍数 -> 要补发的格数。单独拎出来是为了能被单测直接断言换算
   （真注入的条数没法在本进程里稳定观测，见 test_chord 场景Q 的说明）。
   注意只补 (mul-1) 格 —— 物理那一格由系统照常送达（跟手/放大是"加强"，
   不是"替换"），所以"倍数 2"总共恰好是两格。 */
int SendWheelExtraCount(int mul) {
    if (mul <= 1) return 0;
    int extra = mul - 1;
    return extra > WHEEL_EXTRA_MAX ? WHEEL_EXTRA_MAX : extra;
}

void SendWheelAccel(int delta, int mul) {
    SendWheelNotches(delta, SendWheelExtraCount(mul));
}

/* 横向滚动：n 个**固定** ±120 的刻度。这里不沿用物理格的原值而统一成
   WHEEL_STD_DELTA —— 横向是"每次滚轮换一列"的语义，放大倍数才有确定含义
   （倍数 1 = 正好一格），物理格的原始大小（高精度滚轮可能小于 120）不参与。

   **方向（与滚轮上下相反）**：滚轮**向上** -> 向左，向下 -> 向右。
   这不是我们随便定的，而是"Shift + 滚轮"在浏览器 / 资源管理器 / 表格里的
   通用约定（Firefox 官方快捷键表就写着 Shift+上滚 = 向左滚动）。照抄系统
   约定用户才不用重新适应肌肉记忆。 */
void SendWheelHNotches(int delta, int n) { SendWheelHNotchesEx(delta, n, FALSE); }
void SendWheelHNotchesEx(int delta, int n, BOOL hurry) {
    if (delta == 0) return;
    SendWheelAxis((delta > 0) ? -WHEEL_STD_DELTA : WHEEL_STD_DELTA, n, TRUE, hurry);
}

/* 兜底：文字读取失败时，用系统原生 Ctrl(+Shift)+方向键做按词跳转。
   Ctrl 必须在方向键之前按下、之后抬起，否则目标收到的只是单格方向键。 */
void SendNativeWordJump(int dir, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    BOOL hyperHeld = SendHyperSuspend();
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(COMBO_GAP); }
    SendEvent(VK_LCONTROL, FALSE);
    Sleep(COMBO_GAP);
    SendEvent(vk, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, TRUE);
    Sleep(COMBO_GAP);
    SendEvent(VK_LCONTROL, TRUE);
    if (shift) { Sleep(COMBO_GAP); SendEvent(VK_LSHIFT, TRUE); }
    SendHyperResume(hyperHeld);
}

/* 补发一个卡住键的"抬起"。Alt / Win 的"抬起"被系统单独看到时会被解释为
   "按下并释放了 Alt/Win" —— 从而激活菜单栏或开始菜单（AHK 为此专门
   引入 #MenuMaskKey：用一个中性键插在中间，让系统认为中间按过别的键）。
   这里用未定义虚拟键 0xFF 作掩码键，行为与 AHK 的 vkFF 一致。 */
#define MASK_VK 0xFF
static void ReleaseStuck(int vk) {
    if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) {
        SendEvent(MASK_VK, FALSE);
        SendEvent(MASK_VK, TRUE);
    }
    SendEvent(vk, TRUE);
}

/* 重启 / 退出时清键盘卡键（AHK 同理：区分"逻辑状态"与"物理状态"）。
   卡键 = OS 认为某键一直按着，而用户并没有按。两类来源，两种清法：

   ① 本进程注入过 down 还没配 up 的键（g_injMask 有账）——**无条件**补抬。
      这些键本来只该由我们按下，不用看物理表。

   ② 其余"逻辑按下但物理未按"的键 —— **全 VK 扫描**（0x08..0xFE）补抬。
      物理表 g_phys 由键盘钩子维护（hook.c），注入的键不进表，所以注入残留
      即使漏了记账也会在这里被兜住；普通键（字母/方向键）的卡键从此有了
      防线（此前只扫 8 个修饰键，方向键卡键重启永远清不掉）。
      ★ **不看卡键是谁造成的**：别的程序注入后崩掉留下的、驱动/输入法异常
      留下的，只要"OS 认为按着、钩子没见到用户物理按下"就一并清掉 —— 这正
      是重启的"重置键盘"语义（与 KeyCleaner 类工具同效）。
      扫描跳过：
        · 通用别名 VK_SHIFT / VK_CONTROL / VK_MENU —— 左右键各自扫描处理；
          别名的物理表恒为"未按"（钩子只记具体键），扫它会把用户真按着的
          Shift/Ctrl/Alt 误清；
        · 开关键 VK_CAPITAL / VK_NUMLOCK / VK_SCROLL —— 逻辑状态表示
          "灯位"而不是"卡键"，补抬只会把状态搞乱，刻意不处理（CapsLock
          的一贯决定）。
      未定义的 VK（0x07、0x0A..0x0F 等保留段）GetAsyncKeyState 恒为 0，
      扫过去是无害空转，不必逐一排除。

   protectSeededHolds：怎么对待"启动播种的按住"（g_phys 三态里的 2 ——
   钩子装上之前就按着的键，"用户正按着"与"早已卡住"状态同态，见 hook.c）。
     · TRUE（**启动**用）：播种按住视作用户按住、受保护 —— 启动瞬间用户
       真按着修饰键（如登录后按着 Shift）不该被抖掉。代价是"上上次崩溃
       留下的修饰键卡键"在启动这一遍漏清 —— 再重启一次即可（见 FALSE）。
     · FALSE（**重启 / 退出**用）：只保护钩子亲眼见过的按住（g_phys==1）。
       重启是用户显式的"重置键盘"动作，此刻物理按着的键几乎必然已松开，
       播种态的卡键（无论谁造成）一律清掉。

   调用时机与顺序的约束（main.c 各调用点的注释与此互为对照）：
     · 重启路径必须在 ActStop()（执行线程已 join、不再有新注入）之后、
       ModHoldReset()（其中的 PhysSeedFromLogical 会重播种，改变 g_phys
       语义）**之前** —— 排反了就会漏清；
     · 启动路径在 PhysSeedFromLogical() 之后、HookInstall() 之前：注入的
       抬起不会被自己的钩子当成输入处理。
   注：SendInput 返回成功即事件已进入系统输入流，此后进程退出不影响派发
   —— 发完不需要"等一等"。 */
void SendClearStuckModifiers(BOOL protectSeededHolds) {
    BOOL swept[256];
    memset(swept, 0, sizeof(swept));
    /* ① 注入记账的键：无条件补抬起 */
    for (int word = 0; word < 8; word++) {
        LONG bits = InterlockedExchange(&g_injMask[word], 0);
        for (int bit = 0; bit < 32 && bits; bit++) {
            if (bits & ((LONG)1 << bit)) {
                int vk = (word << 5) | bit;
                swept[vk] = TRUE;    /* ② 跳过：异步状态尚未回落，别发两遍 */
                ReleaseStuck(vk);
            }
        }
    }
    /* ② 全 VK 扫描：逻辑按下 && 物理未按 = 卡键（无论卡键是谁造成的） */
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
            continue;
        if (swept[vk]) continue;
        BOOL logicalDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
        BOOL userHeld = protectSeededHolds ? PhysKeyDown(vk)
                                           : PhysKeyObservedDown(vk);
        if (logicalDown && !userHeld) ReleaseStuck(vk);
    }
}

/* ================= Hyper 透传（HyperPassthrough，默认关） =================
   按住 C 的整个和弦期间，替用户按住右侧四修饰键（RWin+RCtrl+RShift+RAlt）：
   未映射键由 hook 放行、带着这组修饰键漏给系统，供 AutoHotkey 等绑定
   >#^!+X 类 Hyper 热键 —— macOS Capslock 的同款方案。开关与语义见 config.c
   的 HyperPassthrough 注释；误触安全性（Win+E/Win+数字等系统热键要求修饰键
   精确匹配，四键齐按时全部失配）已在 test/hyperprobe*.cpp 实测。

   状态与线程（act.c 的意图信箱 -> 本文件的 Apply，跑在**动作线程**上）：
     · g_hyperActive —— 本次和弦启用了 Hyper。动作线程写、钩子线程读。
     · g_hyperReady  —— 四键确实已按下（注入完成后才置位）。就绪之前未映射键
       照旧吞掉 —— 行为不比"关闭"差，只是慢一拍生效（注入是毫秒级）。
     · g_hyperDown / g_hyperDepth —— 只被动作线程触碰（Apply 与
       Suspend/Resume 都在动作线程上串行），普通变量即可。
   两条铁律：
     1) **已映射键的注入必须摘掉四键**（文件前部所有发键/滚轮突发的
        SendHyperSuspend/Resume）：我们的 ← 带着 RWin 出去就是 Win+←，
        直接把目标窗口贴边；RShift 按着时滚轮会被目标当横向/缩放手势。
     2) 四键的注入都走 SendEvent：自动进 g_injMask 记账，退出/重启的
        卡键清理（SendClearStuckModifiers）不用新增路径。 */
static std::atomic<bool> g_hyperActive{ false };
static std::atomic<bool> g_hyperReady{ false };
static BOOL g_hyperDown = FALSE;   /* 仅动作线程读写 */
static int  g_hyperDepth = 0;      /* Suspend 嵌套深度（同上） */

/* 按下 / 抬起四键。无 Sleep：修饰键之间不需要间隔（期间不发别的键，事件流
   顺序即语义）；抬起前插一对掩码键 —— RWin 单独抬起会被系统解释成"按了一下
   Win"弹开始菜单（AHK #MenuMaskKey 同款，与上面 ReleaseMod 同一套）。
   ★ CAP_TEST 下绝不真注入：测试直驱钩子过程 + 直设标志，见 test_chord 场景AI5。 */
static void HyperPress(void) {
#ifdef CAP_TEST
    return;
#else
    SendEvent(VK_RWIN, FALSE);
    SendEvent(VK_RCONTROL, FALSE);
    SendEvent(VK_RSHIFT, FALSE);
    SendEvent(VK_RMENU, FALSE);
#endif
}
static void HyperRelease(void) {
#ifdef CAP_TEST
    return;
#else
    SendEvent(MASK_VK, FALSE);
    SendEvent(MASK_VK, TRUE);
    SendEvent(VK_RMENU, TRUE);
    SendEvent(VK_RSHIFT, TRUE);
    SendEvent(VK_RCONTROL, TRUE);
    SendEvent(VK_RWIN, TRUE);
#endif
}

/* 动作线程调用（act.c 的 Hyper 意图信箱）：on=按下四键并置就绪；
   off=清就绪并抬起。重复调用幂等；collapse 后的最终 wish 决定终态。 */
void SendHyperApply(BOOL on) {
    if (on && CfgBool(CFG_HYPER_PASSTHROUGH)) {
        g_hyperActive = true;
        if (!g_hyperDown) { HyperPress(); g_hyperDown = TRUE; }
        g_hyperReady = true;
    } else {
        g_hyperReady = false;
        g_hyperActive = false;
        if (g_hyperDown) { HyperRelease(); g_hyperDown = FALSE; }
    }
}

/* 发键突发前摘四键 / 结束后按回。嵌套安全（depth 计数）；摘除只看
   active+down，恢复时若和弦已结束（active 被清）就不再按回 —— 松手后
   不该有任何残留的修饰键态。 */
BOOL SendHyperSuspend(void) {
    g_hyperDepth++;
    BOOL was = FALSE;
    if (g_hyperActive.load() && g_hyperDown) {
        HyperRelease();
        g_hyperDown = FALSE;
        was = TRUE;
    }
    return was;
}
void SendHyperResume(BOOL was) {
    if (--g_hyperDepth == 0 && was && g_hyperActive.load() && !g_hyperDown) {
        HyperPress();
        g_hyperDown = TRUE;
    }
}

/* 钩子线程读：未映射键此刻是否该带 Hyper 放行（四键已按下 + 配置开着）。
   放在 send.c 是因为判据的一半（ready）在这里 —— hook.c 不碰状态。 */
BOOL SendHyperLive(void) {
    return g_hyperReady.load() && CfgBool(CFG_HYPER_PASSTHROUGH);
}

#ifdef CAP_TEST
/* 测试直设 ready：不经真注入（CAP_TEST 下 Apply 也不会注入），用于把
   "注入完成"与"未就绪"两个状态确定性地摆出来。 */
void SendHyperTestSetReady(BOOL ready) { g_hyperReady = ready; }
#endif
