/* ================= cap_overlay：子进程底座与按键帮助浮层 =================
   child.c（父死子退 / 子死父知）+ 主程序侧的 help.c
   （拉起、送达对账）。帮助浮层是独立 exe，设置程序也独立但没有子进程关系。
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

/* ---------- 子进程生命周期（帮助浮层的底座；实现在 src/child.c） ----------
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
