/* ================= cap_core：共享常量 + 全局状态 + 日志 =================
   本文件**只被 cap.h 包含**（不自己 include 任何东西，连 windows.h 也不）——
   单独拿去用会在类型上缺依赖。分区与理由见 cap.h 顶部。 */
/* ---------- 共享常量 ---------- */
/* 主线程事件消息（均由钩子线程投递，按 FIFO 顺序处理） */
#define WM_CAPS_DOWN  (WM_APP + 1)  /* C 按下 */
#define WM_CAPS_UP    (WM_APP + 2)  /* C 抬起 */
#define WM_TRIG_DOWN  (WM_APP + 3)  /* 触发键按下   wParam=act，lParam=TRIGF_* 快照 */
#define WM_TRIG_UP    (WM_APP + 4)  /* 触发键抬起   wParam=act，lParam=TRIGF_* 快照 */
#define WM_TOGGLE_TRAY (WM_APP + 5) /* 托盘自定义回调消息 */
#define WM_ALREADY_RUNNING (WM_APP + 7) /* 已有实例在跑：新实例通知它提示后退出 */
#define WM_PROBE       (WM_APP + 6) /* 探测焦点控件（钩子线程投递） */
#define WM_WHEEL_ACCEL (WM_APP + 8) /* Caps+滚轮 快速滚动：wParam=delta，
                                       lParam=低16位倍数 | WM_WHEEL_ACCEL_FULL */
/* lParam 的高位标记：**这一格的物理滚轮已被钩子吞掉**，所以 N 格要全部由
   我们注入（否则物理格 + 补 (N-1) 格才是 N 格）。由钩子在"吞不吞"的同一
   次判定里写下 —— 就绪状态在滚动手势中会变，下游不能重新判。 */
#define WM_WHEEL_ACCEL_FULL 0x10000
#define WM_WHEEL_ENH   (WM_APP + 9) /* Caps+Alt/D+滚轮 持续滚动：wParam=delta */
#define WM_MODE_UP     (WM_APP + 10)/* 某修饰模式的激活键全部抬起：wParam=mode */
#define WM_WHEEL_HACCEL (WM_APP + 11)/* 横向滚动（Caps+Shift+滚轮）：wParam=delta，lParam=倍数。
                                       方向同浏览器：滚轮向上 = 向左。钩子已吞掉物理格。 */
#define WM_RESTART_REQUEST (WM_APP + 12) /* 外部请求重启（--restart / 设置程序保存后） */
#define WM_AUTODISABLE   (WM_APP + 13) /* 钩子 -> 主线程：自动屏蔽状态变化（wParam=1 命中名单） */
#define WM_PIN_ELEVATED (WM_APP + 20) /* 置顶线程 -> 主线程：目标窗口权限更高，SetWindowPos 被 UIPI 拦下 */
#define WM_FOLDER_OPEN (WM_APP + 14) /* 打开所在文件夹**失败**：wParam=FOLDER_ERR_*（一次性线程 -> 主线程弹气泡）。
                                        为什么必须绕回主线程：气泡走 Shell_NotifyIconW + 主线程的 g_nid，
                                        工作线程直接调就是对它的跨线程访问。成功不发消息（不打扰）。 */

/* 托盘气泡被**用户点击**时，uCallbackMessage（WM_TOGGLE_TRAY）收到的通知码。
   官方文档：NIN_BALLOONUSERCLICK = WM_USER + 5 —— 用户点了气泡，程序该在这里做
   那件事（本程序：打开并选中刚写好的日志文件）。
   其余几个同族通知码（NIN_BALLOONTIMEOUT / NIN_BALLOONSHOW / NIN_BALLOONHIDE）
   **不过这几条**：它们只在 balloon 被**内联绘制**进通知区时才会送到 uCallbackMessage，
   走 toast 时系统不发，所以不能拿"NIN_BALLOONTIMEOUT 收到没有"当"气泡超时了"的判据。 */
#define NIN_BALLOONUSERCLICK (WM_USER + 5)

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
#define ACT_OPENFOLDER 15            /* 打开焦点程序所在文件夹并选中该 exe（F3）  */
#define ACT_HELP     16              /* 按键帮助（F1）：拉起/唤起独立 exe CapsEnhanceHelp */
#define ACT_MUTE     18              /* 系统静音开关（F6）：发 VK_VOLUME_MUTE，翻转型 -> 单发 */
#define ACT_VOLDOWN  19              /* 系统音量减（F7）：发 VK_VOLUME_DOWN，步进型 -> 可长按重复 */
#define ACT_VOLUP    20              /* 系统音量加（F8）：发 VK_VOLUME_UP，步进型 -> 可长按重复 */
#define ACT_PGUP     21              /* 上翻页（-）：发 VK_PRIOR，光标/滚动类 -> 可重复、可带选区 */
#define ACT_PGDOWN   22              /* 下翻页（=）：发 VK_NEXT，光标/滚动类 -> 可重复、可带选区 */
#define ACT_PIN      23              /* 窗口置顶开关（T）：翻转类 -> 单发，见 pin.cpp */

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
#define ACTF_OPENFOLDER  0x20   /* 打开所在文件夹：交给 folder.c 的 shell 动作，不走按键发送 */
#define ACTF_HELP        0x40   /* 按键帮助：由 main.c 直接在和弦上开合（见 help.c），不走按键发送 */
#define ACTF_PIN         0x100  /* 窗口置顶：交给 pin.c 的置顶线程（投递窗口句柄），不走按键发送 */

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
     · send.c 的 g_injMask（注入记账位图）—— 方向相反：act.c **工作线程**注入
       键时写（SendEvent），主线程退出 / 重启时读。→ volatile LONG + Interlocked
       （定义在 send.c，不在头文件；Interlocked 原生操作自身线程安全，理由同上）
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

/* ---------- 主窗口 ---------- */
extern HWND g_mainWnd;

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
