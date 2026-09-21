/* ================= cap_input：键位表 / 动作通道 / 钩子 / 发送 =================
   输入链路这一整条：谁把按键翻译成动作（keymap）、动作在哪里被执行
   （act.c 的三条车道）、钩子回调（hook.c）、以及唯一的按键出口（send.c）。
   本文件**只被 cap.h 包含**。 */
/* ---------- 键位映射（src/keymap.c） ----------
   两张声明式表：动作表（触发键 -> 动作 -> 目标键 + 属性标志）与
   修饰模式表（模式 <-> 激活键）。hook.c 与 main.c 只做查询，不再自带表。 */
void KeyMapInit(void);                  /* 启动时装配修饰激活键（配置优先） */
int  ActOfVk(int vk);                   /* 触发键 -> 动作；ACT_NONE = 不是触发键 */
int  ActOutVk(int act);                 /* 动作 -> 目标虚拟键；0 = 不发键（看标志） */
int  ActWordDir(int act);               /* 按词跳转动作 -> DIR_*；其余返回 -1 */
BOOL ActHasFlag(int act, unsigned flag);/* 动作是否带某属性标志 */
const char* ActName(int act);           /* 动作的可读名（仅诊断日志用，不做判定） */

int  ModeFindByVk(int vk);              /* vk 是哪个模式激活键；-1 = 不是 */
void ModeKeyEvent(int mode, int vk, BOOL down); /* 钩子线程：更新按住位图 */
/* 该模式此刻是否生效（任一激活键按住）。
   **这是"模式生效吗"的唯一判据** —— 和弦引擎开火快照（main.c 的 SnapMods）
   与鼠标钩子的滚轮语义判定（hook.c）都读它，两处必须同源。
   此前还有一份"本次和弦内按下过"的 pendMask 门闩，但它与按住位图同步置位、
   同步清除，恒等却单独被引擎复位清掉 —— 结果是"按住 Alt 不放再按 C，点按
   补齐失效"。两份状态只留一份，语义才不会分叉。 */
BOOL ModeDown(int mode);
void ModeResetAll(void);                /* 暂停/重启：清按住状态 */
int  ModeKeyCount(int mode);            /* 配置/测试用 */
int  ModeKeyAt(int mode, int i);
int  ModeKeysText(int mode, WCHAR* out, size_t cap); /* 拼 "Alt, D"（日志/测试） */

/* ---------- 动作执行线程（src/act.c） ----------
   把"会阻塞数百毫秒"的发送动作从主线程挪到专职线程，避免 WH_KEYBOARD_LL
   回调（跑在安装它的主线程上）被挡住 —— 详见 act.c 顶部说明。 */
typedef struct {
    int  act;       /* 要开火的动作（ACT_*）；ACT_NONE = 不开火  */
    int  fillAct;   /* 松手补齐的动作（ACT_*）；ACT_NONE = 不补齐 */
    BOOL enh;       /* 开火快照：增强模式（点按补齐到 3 次）      */
    BOOL shf;       /* 开火快照：选区模式（带 Shift）             */
    int  count;     /* fillAct 的已发送次数（补齐时用）           */
    BOOL keep;      /* 引擎复位（松开 C）时是否保留，见 act.c 的 ActClearKeepFill */
    HWND hwnd;      /* 开火快照：目标窗口（只有 ACTF_OPENFOLDER 用）。
                       捕获时机必须与 enh/shf 一样是"按下触发键的那一刻"（见 main.c 的 PostAct）：
                       执行线程是异步的，等它跑到时前台窗口可能已经换了。 */
    ULONGLONG postTick; /* 投递时刻（GetTickCount64）：诊断打点用，不参与判定 */
} ActJob;

BOOL ActStart(void);              /* 启动执行线程；失败返回 FALSE（退回同步） */
void ActStop(void);               /* 停止并等当前动作结束（退出/重启用）      */
void ActPost(const ActJob* job);  /* 投递一次动作（FIFO）                     */
void ActClear(void);              /* 丢弃未开始的请求（引擎复位）             */
void ActClearKeepFill(void);      /* 同上，但保留"松手补齐"类请求             */
int  ActInFlight(void);           /* 队列中 + 正在执行的动作数                */
void ActDrain(void);              /* 等队列清空且当前动作结束                 */

/* ================= 持续滚动通道（act.c：独立状态 + 独立线程） =================
   **只有「Caps + 增强修饰键（默认 Alt/D） + 滚轮」走这里** —— 滚一下就一直滚，
   与滚轮是否还在动无关（`Caps+滚轮` 是另一回事：它只是瞬时倍数放大，走滚轮通道）。

     · 第一次滚 -> 启动，从 AutoScrollInterval 起步（阅读速度，最慢档）；
     · 同向再滚 -> 逐档提速（几何插值，见 act.c），**最快到 AutoScrollFastMs**；
     · 反向滚   -> 立即终止，并把速度**清零**（下次从最慢档重来）；
     · **松开 Caps / 松开修饰键都不再终止**（2026-09-11 改）：滚起来就能松手读；
     · 终止条件：反向滚、**用户任何新输入**（见下）、**超过 AUTOSCROLL_MAX_MS
       （10 分钟）**、以及暂停映射 / 退出 / 重启。
       超时是为了"人走开了也要能停"：松手之后没有任何输入进来时，它是最后一道闸。

   **什么才算"新输入"（2026-09-11 第二版：只有真的算新输入才打断）**：
     · 按键：**新按下**才算。系统自动重复（typematic）不算 —— 重复的 keydown 在
       按住任意键时都会源源不断，把它算成"新输入"，症状就是"滚一下就停"；
     · 鼠标键按下（左/右/中/X）—— 抬起不算；
     · 滚轮：**只有在没按着 Caps+增强修饰键时才打断**。按着和弦时滚轮是控制杆
       （横向轴、delta=0 的占位帧都算）—— 它们是同一个手势的一部分，不是新输入；
     · 鼠标移动不算（否则手一抖就停）。

   速度一律用"每格多少毫秒"表达，提速靠缩短间隔、不是"每拍补一撮"，所以不会抖。
   默认还开启 **平滑注入**：把一格拆成若干"小数刻度"、提高发数（见 act.c），
   手感接近触控板。
   主线程只投递"发生了一次滚轮"，状态机与注入都在本通道内，主线程不阻塞。
   速度两端（AutoScrollInterval / AutoScrollFastMs）各自独立配置，本通道不读
   WheelScrollMul（倍数量与时间量是两种量，不混用）。 */
void ActAutoScrollWheel(int delta);   /* 主线程：增强模式下的一次滚轮 */
void ActAutoScrollStop(void);         /* 立即终止（暂停 / 退出 / 重启 / 反向滚） */
/* 本次持续滚动是否"只发整格"（前台程序命中 [NoSmoothScroll] 名单）。
   由 ActAutoScrollWheel 在**启动那一刻**按前台程序判定一次并记住：判定要查
   前台进程名（OpenProcess），不能放在每拍的工作线程里，也没必要 —— 滚动
   期间换程序的场景不值得为它每拍付一次系统调用。
   对外暴露是为了可测（单测直接调它就能验证整格/小数刻度两条路）。 */
void ActAutoScrollSetNoSmooth(BOOL on);
BOOL ActAutoScrollNoSmooth(void);
/* 钩子线程：用户有新输入 -> 打断。why/detail 只用于**记录模式下的那行日志**
   （与全项目一致：日志文件只在 `C + \`` 记录期间生成）；要查"它为什么停了"，
   先开始记录再复现，日志里会写清是谁停的与细节（键码 / 滚轮增量 / 鼠标消息号）。 */
void ActAutoScrollInterrupt(const char* why, int detail);
BOOL ActAutoScrollActive(void);       /* 单测用 */
int  ActAutoScrollGear(void);         /* 单测用：当前速度档（1..总档数，未启动为 0） */
int  ActAutoScrollDir(void);          /* 单测用：+1 上滚 / -1 下滚 / 0 未启动 */
int  ActAutoScrollFastMs(void);       /* 单测用：最快档的毫秒/格（= AutoScrollFastMs） */
int  ActAutoScrollTick(void);         /* 单测用：当前速度（每格多少毫秒） */
int  ActAutoScrollBeat(void);         /* 单测用：本拍实际间隔(ms) */
int  ActAutoScrollDelta(void);        /* 单测用：本拍增量（带符号）；平滑模式 < 120（小数刻度） */
int  ActAutoScrollRemainMs(void);     /* 单测用：距离超时还剩多少毫秒（未启动 0） */
void ActAutoScrollTestSetMax(int ms); /* 单测用：缩短超时上限（<=0 回缺省 10 分钟） */

/* ---------- 钩子模块 ---------- */
extern HHOOK g_hook;
BOOL HookInstall(void);
void HookUninstall(void);
BOOL HookMouseAvailable(void); /* 滚轮加速用的鼠标钩子是否装上了（装不上不影响主映射） */
BOOL HookAutoDisabled(void);   /* [AutoDisable] 名单程序是否在前台（上次判定结果）—— 托盘提示重建用 */
BOOL PhysKeyDown(int vk);   /* 真实（非注入）按键是否物理按住 —— 判断卡键用 */
void PhysSeedFromLogical(void); /* 装钩子后：把当前已按下的键视为物理按住，防误清 */
void ModHoldReset(void);    /* 清"按住状态"（C / 真实 Shift / 模式门闩）—— 暂停、重启用 */

/* ---------- 发送/补齐 ---------- */
/* 开火快照（增强/选区模式、已发送次数）由调用方显式传入：实际执行发生在
   act.c 的工作线程上，不能再回头读主线程的 volatile 状态。 */
void SendActOnce(int act, BOOL shift);                          /* 发一次动作 */
void ReleaseFill(int act, BOOL enh, BOOL shift, int count);     /* 松手补齐：增强模式点按 -> 补到3 */

/* ---------- 按键发送模块（src/send.c） ----------
   所有模拟按键集中在这里：扩展键标志、逐条发送节奏、Shift/Ctrl 组合。
   之前 hook.c(SendClick) 与 nav.c(SendArrows/兜底) 各写一份，两边的
   缺陷也不一样（缺 KEYEVENTF_EXTENDEDKEY / Ctrl 提前抬起），统一后只修一处。 */
void SendKeyOnce(int vk, BOOL shift);        /* 单击一个键，shift 全程按住 */

/* SendArrows 的返回值：本次实际走了哪条路（决定上层要不要回读校验落点）。
   为什么要区分：MaxArrowSteps 超限后"改道发原生 Ctrl+方向"只是把跳转交给
   目标程序，落点由**它的**词切分决定 —— 实测 QQ(Chromium/ICU 词典) 的
   Ctrl+方向是 1~2 字一步，而我们按"连续汉字段"整段算，两者不一致时改道会
   把光标丢在段中间。所以改道那一次必须回读并按差额补足；而 OverStepMode=0
   的"只发一格"是既定的"跳一格即停"，不能补。 */
#define ARROW_SEND_STEPS   0   /* 逐格发完 n 步（可做丢键补正）              */
#define ARROW_SEND_HANDOFF 1   /* 改道成原生 Ctrl+方向：落点必须回读校验     */
#define ARROW_SEND_ONE     2   /* 超限且 OverStepMode=0：只发一格，不补正    */
int  SendArrows(int dir, int n, BOOL shift); /* n 个左/右箭头，返回 ARROW_SEND_* */
void SendArrowsStrict(int dir, int n, BOOL shift); /* 同上但绝不改道（补正专用） */
void SendArrowsRequestInterrupt(void);       /* 请求打断正在进行的逐格输出（主线程：钩子回调 / ActStop） */
BOOL SendArrowsWasInterrupted(void);         /* 刚刚那次逐格输出是否被打断 —— 补正阶段据此跳过：
                                                打断 = 和弦已经松手，再发箭头就是在用户松手之后注键 */
BOOL SendArrowsConflicts(int act);           /* act 是否与正在逐格输出的方向冲突 */
void SendNativeWordJump(int dir, BOOL shift);/* 原生 Ctrl(+Shift)+方向：按词跳转兜底 */
void SendWheelAccel(int delta, int mul);     /* 纵向放大：补发 (mul-1) 个同值刻度 */
int  SendWheelExtraCount(int mul);           /* 倍数 -> 补发格数（单测用） */
void SendWheelNotches(int delta, int n);     /* 补发 n 个同值刻度（持续滚动用） */
void SendWheelHNotches(int delta, int n);    /* 横向：n 个 ±120 刻度（Caps+Shift+滚轮） */
#define WHEEL_STD_DELTA 120                  /* 标准滚轮刻度（WHEEL_DELTA） */
void SendClearStuckModifiers(void);          /* 清卡键：逻辑按下且物理未按的补抬起（重启/退出用） */
