/* ================= cap_overlay：子进程底座与两个浮层 =================
   child.c（父死子退 / 子死父知）+ 主程序侧的 help.c / clip.c
   （拉起、送达对账、按键代收）。三个独立 exe 都走这一层。
   本文件**只被 cap.h 包含**。 */
/* ---------- 按键帮助（`C+F1`；窗口在**独立 exe** `CapsEnhanceHelp.exe` 里） ----------
   为什么拆成独立 exe：键位图要一次性叠一大堆 GDI+ 绘制，还要常驻一张位图、
   自己管半透明与生命周期；放进本体就是给它加一份常驻内存与一堆新的失败点，
   而本体是**钩子宿主**（主线程绝不能为了画图分心）。设置程序也是同一条理由。

   寿命 = 和弦的寿命：**按下 F1 显示、抬起（或松开 Caps）即隐**，窗口隐完就退出
   （不常驻 —— 代价是每次 ~20-40ms 冷启动，换零后台占用）。

   ★ 为什么需要"送达重试"（HELP_TIMER 那只定时器）：
   冷启动意味着**每次**显示都是一次 CreateProcess —— 窗口还不存在时主程序
   发的 SHOW/HIDE 会直接丢掉。更糟的是"按下 30ms 就松手"：SHOW 与 HIDE 都在
   窗口出生之前发生，于是窗口起来后不知道该显示还是该消失（旧写法会让它
   永久留在屏幕上）。所以：
     · 帮助 exe 启动后**先建一个隐藏窗口**，等主程序告诉它当前状态；
       1.5 秒内什么都没等到 = 主程序早就不需要它了 → 自己退出（防呆）。
     · 主程序把"期望状态"记下来，用 40ms 的定时器反复对账，直到送达或超时
       （1 秒）。反正状态是幂等的，重复发没有副作用。

   ★★ 寿命兜底不再走心跳（原 HELP_KA_TIMER 已删，完整取舍见 src/child.c 的文件头）：
     · 主程序**死了**（正常退出 / 崩溃 / 被强杀）-> Job Object 由内核收掉帮助
       进程，不需要它配合。
     · 主程序**卡住**（活着但不响应）-> 谁也发不出 HIDE，由帮助 exe 自己的
       3 分钟上限（`TID_LIFE`）兜底。
   第二条是 Job Object 覆盖不到的唯一情形 —— "死亡"是内核事件，"僵住"不是；判僵住
   只能靠超时，而超时判据正是原来那条 800ms 心跳误伤的来源（主线程忙一下就成了
   "父不要我了"）。正常路径下帮助窗口的寿命仍是和弦本身（F1 一抬就 HIDE），上面
   两层只在异常时兜底，且不产生任何周期性流量。手动双击那份本来就没人在喂，3 分钟
   上限对两种模式一视同仁。 */
#define HELP_TIMER   4                    /* 主窗口定时器 id（2=和弦引擎 3=托盘重试） */
#define HELP_TICK_MS 40                   /* 对账间隔 */
#define HELP_TRIES_MAX 25                 /* 40ms x 25 ≈ 1s 内送不到就放弃（下次按下再试） */
#define HELP_CLASS   L"CapsEnhanceHelp"   /* 帮助窗口类名（跨进程握手靠它 FindWindow） */
#define HELP_MSG_SHOW L"CapsEnhanceHelpShow"  /* 注册消息：让它显示 */
#define HELP_MSG_HIDE L"CapsEnhanceHelpHide"  /* 注册消息：让它隐藏并退出 */
#define HELP_EXE     L"CapsEnhanceHelp.exe"
void HelpShow(void);   /* F1 按下：让帮助窗口可见（必要时先拉起它） */
void HelpHide(void);   /* 抬起 / 松开 Caps / 引擎复位：让它隐（它会自己退出） */
void HelpTick(void);   /* HELP_TIMER 到点：把期望状态再送一次（见上面的说明） */

/* ---------- 剪贴板浮层（`Caps+V`；窗口在**独立 exe** `CapsEnhanceClip.exe` 里） ----------
   与帮助窗口同一条理由拆成独立 exe，但比它更重一层：它要打开 Ditto 的 SQLite
   数据库（只读）取最近 30 条，还要在粘贴时把整条记录的多份格式回写进剪贴板 ——
   这些都属于"可能慢、可能失败"的事，绝不能发生在承载钩子的主线程上。

   ★★ 寿命与帮助窗口**不同**：帮助是"按住才显示"（寿命 = 和弦），剪贴板是
   **按一下就开、之后自己活着** —— 按住 V 松手之后窗口还得留着让人挑条目，
   所以这里没有"Caps 抬起就隐"，只有明确的关闭途径（见下）。主程序侧因此
   只做三件事：拉起、转发按键、在它死掉时立刻恢复按键放行。

   关闭途径（浮层自己是 WS_EX_NOACTIVATE，收不到键盘，所以 Esc 只能由主程序的
   键盘钩子代收转发）：
     · Esc            -> 钩子拦下 -> WM_CLIP_ESC -> 主程序通知浮层关闭
     · 点击外部 / 前台窗口变了 -> 浮层用 SetWinEventHook 自己判定
     · 标题栏的 X     -> 浮层自绘，命中测试在它自己那里
     · 再按一次热键   -> toggle（ClipShow 时窗口已在，直接让它关）
   另有一条：**双击标题栏置顶（钉住）**。置顶期间停用"粘贴后自动关"与
   "点击外部自动关"，为的是连粘多条不被打断 —— 置顶态只能靠 X / Esc / 热键退出。

   寿命兜底与帮助窗口同一套（Job Object + 各自的绝对上限），差别只在驻留：浮层收起
   后**进程继续待命 10 分钟**（`GRACE_MS` 在 clip/clip.c），所以"父死子退"这条对它
   更值钱 —— 主程序一退，内核立刻收掉待命中的浮层，不留孤儿。 */
#define CLIP_TIMER   6                    /* 主窗口定时器 id（4=帮助对账） */
#define CLIP_TICK_MS  40                  /* 送达重试间隔 */
#define CLIP_TRIES_MAX 25                 /* 40ms x 25 ≈ 1s 内送不到就放弃 */
#define CLIP_CLASS   L"CapsEnhanceClip"   /* 剪贴板窗口类名（跨进程握手靠它 FindWindow） */
/* 浮层注入按键时打的标记（dwExtraInfo）。浮层是独立 exe，这个值就是跨进程的
   唯一约定，两边共用这一个常量。
   为什么要它：LLKHF_INJECTED 只说明"有程序注入了键"，分不清是谁 —— 浮层粘贴
   发的 Ctrl+V、别的映射工具注入的键，在这一位上是同一个样子。带上标记之后，
   钩子能**精确**认出"这是自家浮层在粘贴"，从而：① 永远不吞它（这条不变量
   从此有代码保证，而不是靠"反正注入的键都在最前面放行"这个位置巧合）；
   ② 记录模式下能把它与用户真实按键分开记，"粘贴没反应"这类问题第一问就是
   "键到底发出去了没有"。 */
#define CLIP_INJECT_MARKER 0x434C4950u    /* 'CLIP' */
#define CLIP_MSG_SHOW L"CapsEnhanceClipShow"  /* 注册消息：让它显示 */
#define CLIP_MSG_HIDE L"CapsEnhanceClipHide"  /* 注册消息：让它隐藏并退出 */
#define CLIP_MSG_ESC  L"CapsEnhanceClipEsc"   /* 注册消息：主程序代收的 Esc */
#define CLIP_MSG_KEY  L"CapsEnhanceClipKey"   /* 注册消息：主程序代收的导航键（wParam=vk，lParam=CLIPKF_*） */
#define CLIP_MSG_CLOSED L"CapsEnhanceClipClosed" /* 注册消息：浮层自行关闭（粘贴/Esc/超时收起）-> 主程序清 s_want */
#define CLIP_EXE     L"CapsEnhanceClip.exe"
void ClipShow(void);    /* Caps+V 按下：显示（已在显示则 toggle 成隐藏） */
void ClipHide(void);    /* 明确关闭（Esc / toggle / 引擎复位） */
void ClipTick(void);    /* CLIP_TIMER 到点：把期望状态再送一次（送达后开始盯它） */
void ClipEsc(void);     /* WM_CLIP_ESC：钩子代收的 Esc -> 通知浮层关闭 */
void ClipCallDitto(void); /* 单击 Caps（浮层开着时）/ 浮层按钮：关浮层并拉起 Ditto 本体 -open */
void ClipKey(int vk, unsigned flags); /* WM_CLIP_KEY：钩子代收的导航键 -> 转给浮层 */
BOOL ClipIsOpen(void);  /* 浮层当前是否"该开着"（钩子据此决定要不要拦 Esc） */
BOOL ClipIsClosedMsg(UINT msg); /* 主窗口收到这条注册消息 = 浮层自行关闭了 */
void ClipClosed(void);  /* 收到 CLIP_MSG_CLOSED：清 s_want，恢复按键放行 */
void ClipGone(void);    /* 收到 WM_CLIP_GONE：浮层进程没了（同一件事，但由内核告知） */

/* ---------- 子进程生命周期（帮助 / 剪贴板浮层的共同底座；实现在 src/child.c） ----------
   两件事：**父死子退**（Job Object，零协作、卡死也照杀）与**子死父知**（进程句柄
   等待，内核通知、零轮询）。原心跳（每 200ms 一条 KA + 对面 800ms 超时自杀）同时
   兼着这两件事、外加一份与子进程自身计时器重复的驻留到期信号，已整体删除。
   完整取舍见 src/child.c 的文件头。 */
typedef struct {
    HANDLE proc;    /* OpenProcess(SYNCHRONIZE) 打开的子进程句柄 */
    HANDLE wait;    /* RegisterWaitForSingleObject 的等待句柄 */
    HWND   notify;  /* 子进程结束时通知哪个窗口 */
    UINT   msg;     /* 用哪条消息通知 */
} ChildWatch;

BOOL ChildSpawn(const WCHAR* exe, WCHAR* cmd, const WCHAR* dir, HANDLE* outProc);
                    /* CreateProcess(CREATE_SUSPENDED) -> 挂进 job -> Resume。
                       outProc 非空时带回进程句柄（调用方负责 CloseHandle）。 */
BOOL ChildWatchPid(ChildWatch* w, DWORD pid, HWND notify, UINT msg);
                    /* 盯着这个 pid：它一结束就给 notify 发 msg。已经在盯则直接返回 */
void ChildWatchStop(ChildWatch* w);
                    /* 拆掉等待（等回调跑完）并复位结构 */
