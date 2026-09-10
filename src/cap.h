#ifndef CAP_H
#define CAP_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ---------- 共享常量 ---------- */
/* 主线程事件消息（均由钩子线程投递，按 FIFO 顺序处理） */
#define WM_CAPS_DOWN  (WM_APP + 1)  /* C 按下 */
#define WM_CAPS_UP    (WM_APP + 2)  /* C 抬起 */
#define WM_TRIG_DOWN  (WM_APP + 3)  /* 触发键按下   wParam=act */
#define WM_TRIG_UP    (WM_APP + 4)  /* 触发键抬起   wParam=act */
#define WM_TOGGLE_TRAY (WM_APP + 5) /* 托盘自定义回调消息 */
#define WM_ALREADY_RUNNING (WM_APP + 7) /* 已有实例在跑：新实例通知它提示后退出 */
#define WM_PROBE       (WM_APP + 6) /* 探测焦点控件（钩子线程投递） */
#define WM_WHEEL_ACCEL (WM_APP + 8) /* 滚轮加速：wParam=原始 delta，lParam=倍数 */
#define WM_MODE_UP     (WM_APP + 10)/* 某修饰模式的激活键全部抬起：wParam=mode */

#define PROBE_KEY      VK_OEM_3    /* 按住 C 时按 ` 触发焦点探测 */

#define CLI_CAPS  VK_CAPITAL         /* 主映射键 C                          */

#define ACT_NONE 0
#define ACT_LEFT 1
#define ACT_RIGHT 2
#define ACT_UP 3
#define ACT_DOWN 4
#define ACT_HOME 5
#define ACT_END 6
#define ACT_BACKSPACE 7
#define ACT_DELETE 8
#define ACT_ENTER 9
#define ACT_CAPSLOCK 10              /* 恢复输出真实 CapsLock                */
#define ACT_WORD_L 11                /* 词跳向左                             */
#define ACT_WORD_R 12                /* 词跳向右                             */
#define ACT_PROBE  13                /* 诊断：探测当前焦点控件                */

#define DIR_LEFT 0
#define DIR_RIGHT 1

/* ---------- 动作属性标志（键位表用，见 src/keymap.c） ----------
   一个动作"能不能重复""要不要补齐""由谁执行""吃不吃 Shift 修饰"都是**数据**，
   不是散落在各处的 if/switch。加一个动作只改键位表一处，不会再出现"改了重复
   逻辑忘了改补齐逻辑"这种漏改。 */
#define ACTF_SINGLE_SHOT 0x01   /* 翻转类：按一次只生效一次，不进重复节奏、不做松手补齐 */
#define ACTF_WORDJUMP    0x02   /* 词跳：交给 nav.c 的 NavJump，不走按键发送 */
#define ACTF_PROBE       0x04   /* 诊断：交给 nav.c 的 NavProbe，不走按键发送 */
#define ACTF_NO_SHIFT    0x08   /* 不吃位移修饰：即使位移模式生效也裸发（开关键用） */

/* ---------- 修饰（和弦）模式 ----------
   按住 C 期间生效的"组合修饰"。每个模式可以由**多个键**激活，具体是哪几个
   键由配置决定（默认值见 MODE_*_KEYS_DEFAULT）。引擎与钩子只问
   "这个模式现在生效吗"，从不判断具体按的是哪个键 ——
   所以"适应了以后把 D 删掉、只留 Alt"只是改一行配置，不用动任何逻辑。 */
#define MODE_ENHANCE 0   /* 增强模式：默认 Alt + D（原"增益"：点按补齐到 3 次、滚轮自动滚动） */
#define MODE_SHIFT   1   /* 位移模式：默认 F（带 Shift 的选区位移） */
#define MODE_COUNT   2

#define MODE_KEY_MAX     8    /* 单个模式最多几个激活键     */
#define MODE_KEYS_MAXLEN 96   /* 激活键列表文本的最大长度    */

/* 默认激活键（ASCII 文本、逗号或空格分隔）。**只在这一处定值**：
   config.c 用它拼配置模板与升级块，keymap.c 用同一套解析器读成虚拟键表 ——
   三处共用一份定义，避免"代码改了、模板/注释没跟着改"的漂移。 */
#define MODE_ENHANCE_KEYS_DEFAULT "Alt, D"
#define MODE_SHIFT_KEYS_DEFAULT   "F"

/* 长按重复节奏 */
#define REPEAT_FIRST_DELAY   150     /* ms：开火到第一次重复                 */
#define REPEAT_INTERVAL      35      /* ms：后续重复间隔                      */
#define CHORD_SETTLE         50      /* ms：和弦稳定期，等增强/位移修饰到位  */

/* ---------- 全局共享状态（钩子线程写，主线程读） ---------- */
typedef struct {
    volatile LONG paused;      /* 1=暂停映射（CapsLock 恢复正常）         */
} GlobalState;

extern GlobalState g_state;

/* 诊断记录开关：C+` 触发开始/结束（主线程读写） */
extern volatile BOOL g_recording;

/* C 键实时状态（volatile，跨线程读：鼠标钩子要据此决定要不要处理滚轮） */
extern volatile BOOL g_cMod;    /* C 按住 */

/* 和弦引擎状态（仅主线程）：开火快照与已发送次数。
   实际发送由 act.c 的工作线程执行，快照随请求一起投递过去。
   enh/shf 是"开火那一刻"的模式快照 —— 命名不叫 D/F，因为按哪个键由配置决定。 */
extern volatile BOOL g_actEnh;  /* 本次动作开火时的 增强模式 快照 */
extern volatile BOOL g_actShf;  /* 本次动作开火时的 位移模式 快照 */
extern volatile int  g_btnCount;/* 本次动作已发送次数 */

/* ---------- 键位映射（src/keymap.c） ----------
   两张声明式表：动作表（触发键 -> 动作 -> 目标键 + 属性标志）与
   修饰模式表（模式 <-> 激活键）。hook.c 与 main.c 只做查询，不再自带表。 */
void KeyMapInit(void);                  /* 启动时装配修饰激活键（配置优先） */
int  ActOfVk(int vk);                   /* 触发键 -> 动作；ACT_NONE = 不是触发键 */
int  ActOutVk(int act);                 /* 动作 -> 目标虚拟键；0 = 不发键（看标志） */
int  ActWordDir(int act);               /* 词跳动作 -> DIR_*；其余返回 -1 */
BOOL ActHasFlag(int act, unsigned flag);/* 动作是否带某属性标志 */

int  ModeFindByVk(int vk);              /* vk 是哪个模式激活键；-1 = 不是 */
void ModeKeyEvent(int mode, int vk, BOOL down); /* 钩子线程：更新按住位图 */
BOOL ModeDown(int mode);                /* 该模式此刻是否生效（任一激活键按住） */
BOOL ModePending(int mode);             /* 本次和弦的门闩（开火快照取它） */
void ModeResetPending(void);            /* 引擎复位：清门闩（按住状态保留） */
void ModeResetAll(void);                /* 暂停/重启：门闩与按住状态一起清 */
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
    BOOL shf;       /* 开火快照：位移模式（带 Shift）             */
    int  count;     /* fillAct 的已发送次数（补齐时用）           */
} ActJob;

BOOL ActStart(void);              /* 启动执行线程；失败返回 FALSE（退回同步） */
void ActStop(void);               /* 停止并等当前动作结束（退出/重启用）      */
void ActPost(const ActJob* job);  /* 投递一次动作（FIFO）                     */
void ActClear(void);              /* 丢弃未开始的请求（引擎复位）             */
void ActClearKeepFill(void);      /* 同上，但保留"松手补齐"类请求             */
int  ActInFlight(void);           /* 队列中 + 正在执行的动作数                */
void ActDrain(void);              /* 等队列清空且当前动作结束                 */

/* 滚轮加速专用通道（独立队列 + 独立线程，见 act.c）。
   ActWheelPost 由主线程调用，绝不阻塞：原来的实现是在主线程 Sleep(2ms)
   逐格补发，主线程不在"等消息"状态期间系统会**跳过**低级钩子，
   于是松开 Caps 的 keyup 可能被漏掉（g_cMod 卡住 → 滚轮停不下来 +
   之后按 Tab 冒出大写锁定），同一根因也是 mul>=3 卡顿的来源。 */
void ActWheelPost(int delta, int mul);   /* 投递一次滚轮加速（非阻塞）      */
void ActWheelClear(void);                /* 丢弃未补发的滚轮请求（松开 C）  */

/* ---------- 钩子模块 ---------- */
extern HHOOK g_hook;
BOOL HookInstall(void);
void HookUninstall(void);
BOOL HookMouseAvailable(void); /* 滚轮加速用的鼠标钩子是否装上了（装不上不影响主映射） */
BOOL PhysKeyDown(int vk);   /* 真实（非注入）按键是否物理按住 —— 判断卡键用 */
void PhysSeedFromLogical(void); /* 装钩子后：把当前已按下的键视为物理按住，防误清 */

/* ---------- 发送/补齐 ---------- */
/* 开火快照（增强/位移模式、已发送次数）由调用方显式传入：实际执行发生在
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
   的"只发一格"是用户明确的"跳一格即停"，不能补。 */
#define ARROW_SEND_STEPS   0   /* 逐格发完 n 步（可做丢键补正）              */
#define ARROW_SEND_HANDOFF 1   /* 改道成原生 Ctrl+方向：落点必须回读校验     */
#define ARROW_SEND_ONE     2   /* 超限且 OverStepMode=0：只发一格，不补正    */
int  SendArrows(int dir, int n, BOOL shift); /* n 个左/右箭头，返回 ARROW_SEND_* */
void SendArrowsStrict(int dir, int n, BOOL shift); /* 同上但绝不改道（补正专用） */
void SendArrowsRequestInterrupt(void);       /* 请求打断正在进行的逐格输出（钩子线程调用） */
BOOL SendArrowsConflicts(int act);           /* act 是否与正在逐格输出的方向冲突 */
void SendNativeWordJump(int dir, BOOL shift);/* 原生 Ctrl(+Shift)+方向：词跳兜底 */
void SendWheelAccel(int delta, int mul);     /* 滚轮加速：补发 (mul-1) 个同值刻度 */
int  SendWheelExtraCount(int mul);           /* 倍数 -> 补发格数（单测用） */
void SendClearStuckModifiers(void);          /* 清卡键：逻辑按下且物理未按的补抬起（重启/退出用） */
void SendForceResetKeys(void);               /* 无条件对所有修饰键补抬起（--reset-keys 强制重置） */

/* ---------- 主窗口 ---------- */
extern HWND g_mainWnd;

/* ---------- 配置模块（CapsEnhance.ini，与 exe 同目录） ---------- */
void ConfigLoad(void);                       /* 启动时读配置；缺失则写注释模板 */
void ConfigLoadFrom(const WCHAR* path);      /* 从指定路径加载（单测用） */
void ConfigUpgradeFile(const WCHAR* path);   /* 老配置补齐新增键（单测用） */
BOOL ConfigMatchName(const WCHAR* exeName);  /* 进程名是否命中原生词跳名单 */
BOOL ConfigGetRunAsAdmin(void);             /* [General] RunAsAdmin：启动时提权 */
int  ConfigGetLogMaxKB(void);               /* [General] LogMaxKB：日志上限(KB)，0=不限 */
int  ConfigGetMaxArrowSteps(void);          /* [General] MaxArrowSteps：逐格词跳最大步数 */
BOOL ConfigGetOverStepCtrl(void);           /* [General] OverStepMode：超限 0=仍发方向键 1=Ctrl+方向 */
int  ConfigGetWheelScrollMul(void);         /* [General] WheelScrollMul：按住 C 时滚轮放大倍数，<2=不放大 */
BOOL ConfigGetStr(const WCHAR* key, WCHAR* out, size_t cap); /* [General] 通用字符串项（未具名解析的） */
BOOL ConfigIsNativeWordJump(HWND focus);    /* 焦点窗口的进程是否命中名单 */
BOOL ConfigGetProcessName(HWND focus, WCHAR* out, size_t cap); /* 焦点窗口的进程 exe 名 */
BOOL IsRunElevated(void);                   /* 真实运行权限（查进程令牌） */

/* ---------- 诊断/日志 ---------- */
void CapLog(const char* fmt, ...);   /* 追加写 CapsEnhance.log（可多线程调用） */
void NavProbe(void);                 /* 探测当前焦点控件并写入日志 + 弹窗 */
void NavBaseInfo(void);              /* 启动时采集系统/环境基线（不落盘，C+` 诊断时输出） */

/* ---------- 词跳模块 ---------- */
int NavJump(int dir, BOOL shift);      /* 前台控件 + dir + shift(位移=F) -> 移动；返回移动格数(<0失败) */
void NavSelTrackReset(void);           /* 清除 Shift 位移序列的锚点跟踪（引擎复位时） */

#endif