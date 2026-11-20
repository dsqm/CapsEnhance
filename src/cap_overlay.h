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

/* ---------- 窗口信息弹窗（`C+F12`；窗口在**独立 exe** `CapsEnhanceInfo.exe` 里）----------
   与按键帮助同一类：本体只负责"拉起 / 告诉它看哪个窗口"，所有查询（窗口树、
   跨进程进程信息）都在子进程里做。差在寿命 —— 帮助窗口是"按住显示、松开即隐"，
   本窗口是持久的（用户要读、要选中、要编辑），按一下只是"换目标并刷新"。
   为什么必须拆进程（而不是在主线程开窗口）、以及送达重试的理由，见 src/info.c。 */
#define INFO_TIMER   5                    /* 主窗口定时器 id（4=帮助） */
void InfoOpen(HWND target);               /* F12 按下：弹窗看这个窗口（已开着则刷新+前置） */
void InfoTick(void);                      /* INFO_TIMER 到点：把目标再送一次 */
BOOL InfoHandleMessage(UINT msg, WPARAM wp, LPARAM lp);
                                          /* 弹窗回存的尺寸消息：吃掉则返回 TRUE */

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

/* ---------- 复制结果预览浮层（`Caps+C`；窗口在本体的**独立线程**里） ----------
   为什么这次不进独立 exe：它是一次性、短寿命、不接收任何输入的提示，没有
   按键图那种常驻位图，也没有窗口信息那种编辑状态。为它加第 5 个 exe 会把
   build.bat / sources.txt / CI 上传清单 / README 一次全撑大，而收益只是
   几十 kb 的隔离 —— 帮助与窗口信息拆进程是因为它们**要自己管生命周期与
   事件循环**，这个浮层只需要一个窗口和一只定时器。

   ★ 采样前必须 ToastHideNow()：浮层就挂在屏幕下方居中，光标恰好停在它上面
     （比如复制任务栏或底部工具栏上的字）时，无障碍命中测试会先命中**我们自己
     的窗口**，取回一堆提示文字。这是"浮层会污染取样"那条坑的落地。 */
void ToastHideNow(void);   /* 同步隐藏（SendMessage 到窗口线程，返回时已隐藏） */
void ToastShow(BOOL fail, const WCHAR* title, const WCHAR* meta, const WCHAR* body);
                           /* 显示一次提示：title = 顶部小字（来源程序 / 失败标题），
                              meta = **标题行右侧**的计数（"124 字 · 5 行"；NULL 或
                              空串 = 不显示，标题独占整行）。它只影响窗口宽度，
                              从不增加高度；标题过长时按省略号截断，两者不重叠。
                              body = 正文预览。
                              几何/字体/透明度读 [General] 的 Copy* 键；
                              fail 只决定强调色与停留时长取哪一个。 */
