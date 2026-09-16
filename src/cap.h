#ifndef CAP_H
#define CAP_H

#define WIN32_LEAN_AND_MEAN

/* ---------- C++ 迁移桥：CINTERFACE + COBJMACROS ----------
   本项目现在**按 C++ 编译**（构建脚本用 /TP）。但 COM 代码（nav.c 的 UIA/IA2、
   autodisp.c 的 IDispatch、wps.c 的附着）是按 C 的 `p->lpVtbl->Method(p, ...)`
   习惯写的，而 C++ 下 SDK 默认给的是"带虚函数的 C++ 接口"——`->lpVtbl->` 与
   `IUIAutomationXxx_Release(p)` 这类宏在那边都不存在（约 70 处调用会一起报错）。

   定义 CINTERFACE 让 SDK 继续给出 C 形态的 vtable 结构体，COBJMACROS 提供
   `Xxx_Method(p, ...)` 宏包装 —— 同一份源码在 C 与 C++ 下都能编过。这是"先把
   编译面切到 C++，再逐模块换成 C++ 写法"的过渡层；换完之后本块即可删除。
   （要不要换成 `p->Method(...)` + ComPtr 那套 C++ COM 写法，是独立的一步，
   与"按 C++ 编译"无关 —— 后者只要求能编过，前者才是风格统一。）

   **必须出现在任何 SDK 头之前**，所以放在这里：cap.h 是所有 TU 的第一个包含
   （包括直接 `#include "nav.c"` 的 test_sci.c / memtest.c）。不写成
   `/DCINTERFACE` 编译开关，是为了让"为什么需要它"留在源码里、而不是散在构建脚本。

   注意 REFIID / REFCLSID / REFGUID 在 C++ 下**恒为引用类型**（与 CINTERFACE
   无关，见 guiddef.h），所以传 IID 时直接写 `IID_XXX`，不能写 `&IID_XXX`。 */
#ifdef __cplusplus
#define CINTERFACE
#define COBJMACROS
#endif

#include <windows.h>
#include <atomic>   /* g_recording：真跨线程标志，见下 */

/* ---------- 共享常量 ---------- */
/* 主线程事件消息（均由钩子线程投递，按 FIFO 顺序处理） */
#define WM_CAPS_DOWN  (WM_APP + 1)  /* C 按下 */
#define WM_CAPS_UP    (WM_APP + 2)  /* C 抬起 */
#define WM_TRIG_DOWN  (WM_APP + 3)  /* 触发键按下   wParam=act，lParam=TRIGF_* 快照 */
#define WM_TRIG_UP    (WM_APP + 4)  /* 触发键抬起   wParam=act，lParam=TRIGF_* 快照 */
#define WM_TOGGLE_TRAY (WM_APP + 5) /* 托盘自定义回调消息 */
#define WM_ALREADY_RUNNING (WM_APP + 7) /* 已有实例在跑：新实例通知它提示后退出 */
#define WM_PROBE       (WM_APP + 6) /* 探测焦点控件（钩子线程投递） */
#define WM_WHEEL_ACCEL (WM_APP + 8) /* Caps+滚轮 快速滚动：wParam=delta，lParam=倍数 */
#define WM_WHEEL_ENH   (WM_APP + 9) /* Caps+Alt/D+滚轮 持续滚动：wParam=delta */
#define WM_MODE_UP     (WM_APP + 10)/* 某修饰模式的激活键全部抬起：wParam=mode */
#define WM_WHEEL_HACCEL (WM_APP + 11)/* 横向滚动（Caps+Shift+滚轮）：wParam=delta，lParam=倍数。
                                       方向同浏览器：滚轮向上 = 向左。钩子已吞掉物理格。 */
#define WM_RESTART_REQUEST (WM_APP + 12) /* 外部请求重启（--restart / 设置程序保存后） */
#define WM_AUTODISABLE   (WM_APP + 13) /* 钩子 -> 主线程：自动屏蔽状态变化（wParam=1 命中名单） */

/* ---------- 单键切换输入法（src/ime.c） ----------
   三个"单键"各配一个模式值（IME_MODE_*），配置里是 [General] 的
   ImeCapsMode / ImeLShiftMode / ImeRShiftMode。
   为什么是"每键一个模式值"而不是三个布尔开关：三个键的差别只有"切到哪一边"
   这一个维度，做成数据之后加一个键只加一行常量，判定代码一处不动 —— 与
   keymap.c"属性写在表里、不在逻辑里列举"是同一条规矩。 */
#define IME_MODE_OFF 0   /* 不处理 */
#define IME_MODE_CN  1   /* 强制切到中文 */
#define IME_MODE_EN  2   /* 强制切到英文 */

#define IME_KEY_CAPS   0   /* 单击 CapsLock */
#define IME_KEY_LSHIFT 1   /* 单击左 Shift  */
#define IME_KEY_RSHIFT 2   /* 单击右 Shift  */
#define IME_KEY_COUNT  3

/* Shift「单击」的时长上限：按下到抬起超过它就算"当修饰键用"，不切输入法。
   Caps **不需要**这个上限 —— 它是和弦首键，松手时"本次会话没触发过任何动作"
   就等价于单击，按多久都算。
   放在这里而不是做成配置项：它是"点一下"这个手势的定义，不是口味旋钮
   （与 REPEAT_FIRST_DELAY / REPEAT_INTERVAL 同一类）。 */
#define IME_TAP_MS 300

#define PROBE_KEY      VK_OEM_3    /* 按住 C 时按 ` 触发焦点探测 */

/* WM_TRIG_DOWN / WM_TRIG_UP 的 lParam 快照位（钩子在**事件发生的那一刻**取的
   修饰状态；按下那条决定本次动作带不带增强/选区）。
   为什么必须随消息带过来、而不能等主线程处理消息时现读：钩子回调与消息
   处理是**异步**的 —— 用户"点按 J 之后立刻松开 Alt"时，Alt 的抬起已经在
   钩子里清掉了按住位图，而这条消息还排在队列里；主线程现读就会读到
   "Alt 已松"，该带增强的那一发变成裸动作。详见 hook.c 的 ModeSnapFlags。 */
#define TRIGF_ENH     0x1           /* 事件那一刻 增强模式生效 */
#define TRIGF_SHF     0x2           /* 事件那一刻 选区模式生效 */

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
#define ACT_WORD_L 11                /* 按词跳转向左                             */
#define ACT_WORD_R 12                /* 按词跳转向右                             */
#define ACT_PROBE  13                /* 诊断：探测当前焦点控件                */
#define ACT_MOVEWIN 14               /* 把焦点窗口挪到鼠标光标处（多显示器）  */

#define DIR_LEFT 0
#define DIR_RIGHT 1

/* ---------- 动作属性标志（键位表用，见 src/keymap.c） ----------
   一个动作"能不能重复""要不要补齐""由谁执行""吃不吃 Shift 修饰"都是**数据**，
   不是散落在各处的 if/switch。加一个动作只改键位表一处，不会再出现"改了重复
   逻辑忘了改补齐逻辑"这种漏改。 */
#define ACTF_SINGLE_SHOT 0x01   /* 翻转类：按一次只生效一次，不进重复节奏、不做松手补齐 */
#define ACTF_WORDJUMP    0x02   /* 按词跳转：交给 nav.c 的 NavJump，不走按键发送 */
#define ACTF_PROBE       0x04   /* 诊断：交给 nav.c 的 NavProbe，不走按键发送 */
#define ACTF_NO_SHIFT    0x08   /* 不吃选区修饰：即使选区模式生效也裸发（开关键用） */
#define ACTF_MOVEWIN     0x10   /* 挪窗口：交给 winmove.c 的移动动作，不走按键发送 */

/* ---------- 修饰（和弦）模式 ----------
   按住 C 期间生效的"组合修饰"。**激活键是编译期常量，不读配置**（见下面
   MODE_*_KEYS_DEFAULT）：这两个键属于"和弦语法"本身，改它等于改功能，
   不该是暴露给配置文件的旋钮 —— 配置文件里即使写了也一律忽略。

   但"模式"这层抽象保留：引擎与钩子只问"这个模式现在生效吗"，从不判断
   具体按的是哪个键，所以以后想换/删某个激活键，只改这一处常量即可，
   不用碰任何逻辑（keymap.c 的查询接口一律不变）。 */
#define MODE_ENHANCE 0   /* 增强模式：激活键 Alt + D（点按触发键补齐到 3 次；配合滚轮自动滚动） */
#define MODE_SHIFT   1   /* 选区模式：激活键 F（带 Shift 的选区位移） */
#define MODE_COUNT   2

#define MODE_KEY_MAX     8    /* 单个模式最多几个激活键     */
#define MODE_KEYS_MAXLEN 96   /* 激活键列表文本的最大长度    */

/* 激活键（ASCII 文本、逗号或空格分隔）。**只在这一处定值**：
   keymap.c 用解析器把它读成虚拟键表，main.c 的日志回显同一份，避免漂移。 */
#define MODE_ENHANCE_KEYS_DEFAULT "Alt, D"
#define MODE_SHIFT_KEYS_DEFAULT   "F"

/* 长按重复节奏 */
#define REPEAT_FIRST_DELAY   150     /* ms：开火到第一次重复                 */
#define REPEAT_INTERVAL      35      /* ms：后续重复间隔                      */
/* 这里原来有一个 CHORD_SETTLE(50ms) 和弦稳定期：按下触发键后先等 50ms 再开火，
   用来兜住"触发键先到、修饰键后到"的乱序（J 和 Alt 几乎同时按）。代价是**每个**
   触发键都白等 50ms 才响应（实测按下到开火 57~71ms）。现已删除：顺序语义明确为
   **修饰键必须先按住，再按触发键**，按下即开火，见 main.c 的 SnapMods。 */

/* ---------- 跨线程状态：atomic / volatile 的分界 ----------
   **volatile 不是同步原语** —— 在 C/C++ 内存模型里它只阻止编译器把访问优化
   掉，既不建立 happens-before、也不保证原子性。所以"跨线程用的标志"不能写
   volatile，要么 std::atomic、要么由一把锁 / 一次 join 建立顺序。

   本项目里**真正跨线程**的用户态标志共有三组：

     · g_recording —— 主线程写（C+` 切换），而 act.c / send.c / nav.c 的
       **工作线程**读它，决定要不要写诊断日志。→ std::atomic<bool>（本文件）
     · send.c 的 g_injectedDown —— 方向相反：act.c **工作线程**注入修饰键时写，
       主线程退出 / 重启时读。→ std::atomic<bool>（定义在 send.c，不在头文件）
     · send.c 的逐格打断标志 g_arrBusy / g_arrDir / g_arrBreak —— 前两个 act.c
       **工作线程**写（ArrowsEmit）、主线程钩子读（SendArrowsConflicts）；
       g_arrBreak 反过来（主线程写、工作线程读）。→ 仍用 volatile：它们是
       "意图"标志，最坏只是晚一拍响应，不值得付 atomic 的代价

   其余在这里声明为 volatile 的标志（g_cMod / g_shiftMod / g_state.paused /
   g_actEnh / g_actShf / g_btnCount）**其实都不跨线程**：两个低级钩子都由主线程
   SetWindowsHookEx 安装，于是键盘回调、鼠标回调与窗口消息处理**全在主线程上
   串行**；而工作线程刻意不读它们（开火快照随 ActJob 传值过去，见 act.c）。

   那这些 volatile 为什么留着 —— 因为低级钩子回调是**异步进入**的，编译器
   从源码看不出它会在任意时刻被系统调用，于是可能把 `while (x) { ... }` 里的
   读取提升到循环外。volatile 正是用来禁止这种优化的。
   **一句话：volatile 在这里防的是"编译器优化"，不是"线程竞争"。** */

/* ---------- 全局共享状态（钩子回调与消息处理同在主线程，串行） ---------- */
typedef struct {
    volatile LONG paused;      /* 1=暂停映射（CapsLock 恢复正常）         */
} GlobalState;

extern GlobalState g_state;

/* 诊断记录开关：C+` 触发开始/结束。
   **只由主线程写**，但工作线程会读（见上面"atomic / volatile 的分界"）——
   这是本文件里唯一真正跨线程的标志，所以用 std::atomic 而不是 volatile。 */
extern std::atomic<bool> g_recording;

/* C 键实时状态。写法是 volatile 而非 atomic —— 因为**它们不跨线程**：
   键盘钩子写、鼠标钩子读，而两个钩子都在主线程上（串行）。volatile 在这里
   只是防止编译器把读取提出循环（回调异步进入，编译器看不出来）。
   （cap.h 旧注释曾写"跨线程读：鼠标钩子要据此决定…"，那是误导 —— 鼠标钩子
   与键盘钩子同线程，不存在竞态。） */
extern volatile BOOL g_cMod;       /* C 按住 */
extern volatile BOOL g_shiftMod;   /* 真实 Shift 按住（Caps+Shift+滚轮 = 横向滚动） */

/* 和弦引擎状态（仅主线程）：开火快照与已发送次数。
   实际发送由 act.c 的工作线程执行，快照随请求一起投递过去。
   enh/shf 是"开火那一刻"的模式快照 —— 命名不叫 D/F，因为按哪个键由配置决定。 */
extern volatile BOOL g_actEnh;  /* 本次动作开火时的 增强模式 快照 */
extern volatile BOOL g_actShf;  /* 本次动作开火时的 选区模式 快照 */
extern volatile int  g_btnCount;/* 本次动作已发送次数 */

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
    ULONGLONG postTick; /* 投递时刻（GetTickCount64）：诊断打点用，不参与判定 */
} ActJob;

BOOL ActStart(void);              /* 启动执行线程；失败返回 FALSE（退回同步） */
void ActStop(void);               /* 停止并等当前动作结束（退出/重启用）      */
void ActPost(const ActJob* job);  /* 投递一次动作（FIFO）                     */
void ActClear(void);              /* 丢弃未开始的请求（引擎复位）             */
void ActClearKeepFill(void);      /* 同上，但保留"松手补齐"类请求             */
int  ActInFlight(void);           /* 队列中 + 正在执行的动作数                */
void ActDrain(void);              /* 等队列清空且当前动作结束                 */

/* ---------- 窗口挪到光标处（src/winmove.c） ----------
   一个动作两个用途：多显示器之间搬窗口；把跑到屏幕外的窗口拉回来。
   落点 = 窗口中心对齐光标，再整体钳进"光标所在显示器的工作区"。 */
void WinComputePlacement(const RECT* work, POINT cursor, int w, int h, POINT* out);
                        /* 纯函数：算左上角落点。单测用它覆盖多显示器几何（不碰真实窗口） */
BOOL WinMoveWindowToCursor(HWND hwnd);
                        /* 移动指定窗口（测试与实现共用；不自己去查前台窗口） */
BOOL WinMoveFocusedToCursor(void);
                        /* 动作入口：把焦点程序的**顶层**窗口挪到光标处 */
BOOL WinIsShellClass(HWND hwnd);
                        /* 桌面 / 任务栏 / 通知区那类"不是普通窗口"的壳窗口（winmove 与 drag 共用） */

/* ---------- 坐标空间（进程级，必须在建任何窗口之前调用一次） ----------
   ★ 根因记录：低级鼠标钩子给的 `MSLLHOOKSTRUCT.pt` **永远是物理像素**
   （官方定义就是 "per-monitor-aware screen coordinates"），而**不声明 DPI 感知**的
   进程里 `GetCursorPos` / `WindowFromPoint` / `GetWindowRect` / `SetWindowPos` 用的
   是系统按 96DPI **虚拟化**过的坐标。两者不在一个空间 —— 本机实测（真实 2558x1319
   @175%）：钩子给 (1351,613)，而不感知的进程里 `GetCursorPos` 是 (772,350)。
   于是 `WindowFromPoint(钩子坐标)` 会指到别的地方（表现为"只能拖有焦点的窗口"），
   而光标位移（物理量）加到虚拟坐标上会让窗口多跑 75%（表现为"跟光标差很远"）。
   对策与 AltSnap 一致：**声明 Per-Monitor V2**，让两边都落在物理空间。
   调用点：`WinMain` 的第一件事；测试可单独调用（见 test_chord 末尾）。 */
void WinInitDpiAwareness(void);
BOOL WinCoordSpaceConsistent(void);
                        /* 自检：本进程坐标空间是否与钩子一致（GetCursorPos vs
                           GetPhysicalCursorPos）。不一致说明 DPI 感知没生效 ——
                           拖动/挪窗口都会错位，启动时写日志报警。 */
const WCHAR* WinDpiAwarenessName(void);
                        /* 当前 DPI 感知级别名（仅日志/诊断用，不做判定） */

/* ---------- Caps + 左键拖动挪窗口（src/drag.c） ----------
   钩子只做"判定 + 记录 + 唤醒"（鼠标输入的关键路径上不能阻塞），
   SetWindowPos 由本模块的专用线程执行（与滚轮 / 持续滚动同一套通道模型）。
   判定与落点规则借鉴 AltSnap（它默认用 Alt 键做同一件事）。 */
BOOL DragBegin(POINT pt);   /* 钩子：按下那一刻判定是否接管；TRUE = 接管（调用方必须吞掉这次按下） */
void DragMove(POINT pt);    /* 钩子：更新最新光标位置并唤醒执行线程（每帧一次内存写 + SetEvent） */
void DragEnd(void);         /* 钩子：结束本次拖动（执行线程会补上最后一次落点） */
BOOL DragActive(void);      /* 是否正在拖动（键盘钩子"松开 Caps 就收手"要用） */
void DragLaneStart(void);   /* act.c 的 ActStart 调用（起不来则该功能静默不可用） */
void DragLaneStop(void);    /* act.c 的 ActStop 调用 */
void DragComputeTopLeft(POINT wndTL, POINT from, POINT to, POINT* out);
                        /* 纯函数：落点 = 按下时的窗口左上角 + 光标位移（单测覆盖） */

/* 滚轮加速专用通道（独立队列 + 独立线程，见 act.c）。
   ActWheelPost 由主线程调用，绝不阻塞：原来的实现是在主线程 Sleep(2ms)
   逐格补发，主线程不在"等消息"状态期间系统会**跳过**低级钩子，
   于是松开 Caps 的 keyup 可能被漏掉（g_cMod 卡住 → 滚轮停不下来 +
   之后按 Tab 冒出大写锁定），同一根因也是 mul>=3 卡顿的来源。 */
void ActWheelPost(int delta, int mul);   /* 投递一次纵向放大（Caps+滚轮），非阻塞 */
void ActWheelPostH(int delta, int n);    /* 投递一次横向滚动（Caps+Shift），注入 n 格 */
void ActWheelClear(void);                /* 丢弃未补发的滚轮请求（松开 C）  */

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
BOOL SendArrowsConflicts(int act);           /* act 是否与正在逐格输出的方向冲突 */
void SendNativeWordJump(int dir, BOOL shift);/* 原生 Ctrl(+Shift)+方向：按词跳转兜底 */
void SendWheelAccel(int delta, int mul);     /* 纵向放大：补发 (mul-1) 个同值刻度 */
int  SendWheelExtraCount(int mul);           /* 倍数 -> 补发格数（单测用） */
void SendWheelNotches(int delta, int n);     /* 补发 n 个同值刻度（持续滚动用） */
void SendWheelHNotches(int delta, int n);    /* 横向：n 个 ±120 刻度（Caps+Shift+滚轮） */
#define WHEEL_STD_DELTA 120                  /* 标准滚轮刻度（WHEEL_DELTA） */
void SendClearStuckModifiers(void);          /* 清卡键：逻辑按下且物理未按的补抬起（重启/退出用） */

/* ---------- 主窗口 ---------- */
extern HWND g_mainWnd;

/* ---------- 配置模块（CapsEnhance.ini，与 exe 同目录） ---------- */
/* 名单编号。五份名单语义完全不同（改通道 / 整段不介入 / 只改滚动粒度 /
   拖动时按窗口类放过 / 拖动时按进程放过），所以是"五份"而不是"一份 + 几个开关"。
   编号放在这里是为了让 cap.h 就能表达"查哪份名单"（ConfigMatchList），
   而不必把 config.c 的枚举暴露出去。 */
#define CFG_LIST_NATIVE      0   /* [NativeWordJump]：不做文本探测，直接发 Ctrl+←/→ */
#define CFG_LIST_AUTODISABLE 1   /* [AutoDisable]：前台是它时整段不介入          */
#define CFG_LIST_NOSMOOTH    2   /* [NoSmoothScroll]：持续滚动只发整格           */
#define CFG_LIST_DRAGWIN     3   /* [DragWindowBlacklist]：按窗口类名，不许拖动  */
#define CFG_LIST_DRAGPROC    4   /* [DragProcessBlacklist]：按进程名，不许拖动    */
#define CFG_LIST_COUNT       5

/* 名单容量：设置程序要用它来定界面里的数组，所以放在这里而不是 config.c
   （同一份判据，别在两处各写一个 128 / 64）。 */
#define CFG_MAX_ENTRIES 128   /* 每份名单最多多少条 */
#define CFG_NAME_MAX    64    /* 单条进程名的最大字符数（含结尾的 0） */

void ConfigLoad(void);                       /* 启动时读配置；缺失则写注释模板 */
void ConfigLoadFrom(const WCHAR* path);      /* 从指定路径加载（单测用） */
void ConfigUpgradeFile(const WCHAR* path);   /* 老配置补齐新增键（单测用） */
/* 升级键清单（只读）：单测据此**自动**生成"键齐全"的 fixture ——
   手写 fixture 时每加一个配置项都要记得同步，漏了就变成假失败。 */
int  ConfigUpgradeKeyCount(void);
const char* ConfigUpgradeKeyName(int i);     /* 越界返回 NULL */
BOOL ConfigMatchName(const WCHAR* exeName);  /* 进程名是否命中原生按词跳转名单 */
BOOL ConfigGetRunAsAdmin(void);             /* [General] RunAsAdmin：启动时提权 */
int  ConfigGetLogMaxKB(void);               /* [General] LogMaxKB：日志上限(KB)，0=不限 */
int  ConfigGetMaxArrowSteps(void);          /* [General] MaxArrowSteps：逐格按词跳转最大步数 */
BOOL ConfigGetOverStepCtrl(void);           /* [General] OverStepMode：超限 0=仍发方向键 1=Ctrl+方向 */
int  ConfigGetWheelScrollMul(void);         /* [General] WheelScrollMul：Caps+滚轮 放大倍数，<2=不放大 */
int  ConfigGetWheelHScrollMul(void);        /* [General] WheelHScrollMul：Caps+Shift+滚轮 横向倍数，0=关闭 */
int  ConfigGetAutoScrollInterval(void);     /* [General] AutoScrollInterval：持续滚动初始速度(ms/格) */
int  ConfigGetAutoScrollFastMs(void);        /* [General] AutoScrollFastMs：持续滚动最快速度(ms/格) */
int  ConfigGetAllowScintillaRead(void);     /* [General] AllowScintillaRead：允许 Scintilla 通道读目标进程内存（0=关闭该通道） */
/* 单键切换输入法：key 为 IME_KEY_*，返回 IME_MODE_*（0=不处理 1=中文 2=英文） */
int  ConfigGetImeMode(int key);
BOOL ConfigIsNativeWordJump(HWND focus);    /* 焦点窗口的进程是否命中 [NativeWordJump] */
BOOL ConfigIsAutoDisabled(HWND focus);      /* 前台窗口的进程是否命中 [AutoDisable]（命中 = 整段不介入） */
BOOL ConfigIsDragBlacklisted(HWND hwnd);    /* 是否命中两份拖动黑名单之一（[DragWindowBlacklist] 按类名 / [DragProcessBlacklist] 按进程名） */
BOOL ConfigIsNoSmoothScroll(HWND focus);    /* 前台窗口的进程是否命中 [NoSmoothScroll]（命中 = 持续滚动只发整格） */
BOOL ConfigMatchList(int list, const WCHAR* exeName); /* 进程名是否命中指定名单（CFG_LIST_*） */
BOOL ConfigGetProcessName(HWND focus, WCHAR* out, size_t cap); /* 焦点窗口的进程 exe 名 */
BOOL IsRunElevated(void);                   /* 真实运行权限（查进程令牌） */

/* ---- 设置程序（settings/settings.c）用：改值 + 增量写回 ----
   只给设置程序用：它改的是"某个配置项"，不是整份文本。钳制与解析同源
   （config.c 的 Clamp*），写回保留注释与未知段（规则见 ConfigSaveFile）。
   新增配置项时这里也要加一个 Set —— 与 Get 成对，漏了就是"界面改不动它"。 */
void ConfigSetRunAsAdmin(BOOL v);
void ConfigSetLogMaxKB(int v);
void ConfigSetMaxArrowSteps(int v);
void ConfigSetOverStepCtrl(BOOL v);
void ConfigSetWheelScrollMul(int v);
void ConfigSetAutoScrollInterval(int v);
void ConfigSetAutoScrollFastMs(int v);
void ConfigSetWheelHScrollMul(int v);
void ConfigSetAllowScintillaRead(BOOL v);
void ConfigSetImeMode(int key, int v);      /* key = IME_KEY_*，v = IME_MODE_*（越界按键忽略） */
void        ConfigSetList(int list, const WCHAR* const* names, int n); /* 整份替换 */
int         ConfigListCount(int list);
const WCHAR* ConfigListAt(int list, int i);
BOOL ConfigSaveFile(const WCHAR* path);      /* 写回 ini；失败返回 FALSE（文件不落盘） */

/* ---------- 诊断/日志 ---------- */
/* 日志写在 exe 同目录的 log\ 子目录里：一次记录一个文件
   （CapsEnhance-<YYYYMMDD>-<HHMMSS>.log），开始记录时由 LogNewSession 新建，
   所以 REC START 一定是该文件的第一行；还没开始记录时的零星日志落在
   兜底的 log\CapsEnhance.log。细节见 main.c 日志一节。 */
void CapLog(const char* fmt, ...);   /* 追加写当前会话日志（可多线程调用） */
void LogNewSession(void);            /* 开始记录时调用：新建一个以时间命名的日志文件 */
void NavProbe(void);                 /* 探测当前焦点控件并写入日志 + 弹窗 */
void NavBaseInfo(void);              /* 启动时采集系统/环境基线（不落盘，C+` 诊断时输出） */
void NavBaseAppendLine(const WCHAR* text);   /* 往基线里追加一行（main.c 补 DPI/坐标空间自检用） */

/* ================= 输入法单键切换（src/ime.c） =================
   **单击** Caps / 左 Shift / 右 Shift 时，无视当前输入法状态强制切到中文或英文
   （对应 KBLAutoSwitch 的"特殊热键"、InputTip 的 IME.SetInputMode）。

   判"单击"在 hook.c（那里才看得到每一次按下/抬起），真正干活在**本模块的专用
   线程**上 —— 切换要向别的进程的 IME 窗口连发 3 条 SendMessageTimeout
   （每条最多等 200ms），跑在承载钩子的主线程上就是把键盘冻住几百毫秒，
   那正是 act.c 存在的原因。

   两条闸门（都在这条线程里判，所以钩子路径上不增加任何系统调用）：
     · **英文键盘（主语言 0x09）一律不处理** —— 三个键都算，Caps 也一样。
       本程序只切"当前输入法"的中/英状态，**不替用户换键盘布局**（那等于改他的
       输入环境，与"不写注册表、不改系统设置"的边界相冲突）。所以停在英文键盘
       上时按这三个键没反应，是刻意的行为，不是失灵。
     · [AutoDisable] 名单：前台程序命中即不介入（与"Caps 整段不介入"同一份名单）。
   注：这里与 KBLAutoSwitch / InputTip 的做法**不同** —— 它们会主动把键盘布局切到
   中文（InputTip 甚至会"没装就装一个"），本项目刻意不做这一步。 */
void ImeRequest(int key);   /* 钩子：IME_KEY_* 被单击了。**只投递，绝不阻塞** */
void ImeLaneStart(void);    /* act.c 的 ActStart 调用 */
void ImeLaneStop(void);     /* act.c 的 ActStop 调用 */

/* ---------- 按词跳转模块 ---------- */
int NavJump(int dir, BOOL shift);      /* 前台控件 + dir + shift(选区=F) -> 移动；返回移动格数(<0失败) */
long long NavFocusCaret(void);         /* 诊断用：读焦点控件当前光标（读不到 -1），只在记录模式下调 */
void NavSelTrackReset(void);           /* 清除 Shift 位移序列的锚点跟踪（引擎复位时） */
int  ComputeSteps(const WCHAR* s, int len, int caret, int dir); /* 词边界算法，仅供单测 */

/* ---------- 自动化 / 降权桥 / WPS 通道 ----------
   这三个模块的接口都在 **src/auto.h**，不放在这里：它们的签名要用到
   IDispatch/VARIANT（需要 <oaidl.h>），而 cap.h 被所有 TU 包含 ——
   钩子路径上的文件不该为了几条无关的 COM 声明多拉一个 SDK 头。
   分工见 auto.h 顶部；nav.c 只把 WPS 当成"又一条文本通道"调用。 */

#endif