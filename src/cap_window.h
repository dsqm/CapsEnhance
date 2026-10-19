/* ================= cap_window：窗口几何 =================
   winmove（挪到光标）/ drag（Caps+左键拖动）/ folder（打开所在文件夹）/
   pin（窗口置顶），
   外加"坐标空间必须=物理像素"这条进程级前提的初始化与自检。
   本文件**只被 cap.h 包含**。 */
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
BOOL WinIsFullscreen(HWND hwnd);
                        /* 窗口矩形 == 它所在显示器的**整屏**矩形（rcMonitor，不含任务栏那一侧的
                           收缩）。判定**独占全屏/全屏游戏/全屏视频**用。drag（C+拖动）与
                           winmove（C+Q）共用同一条标准 —— 两边都该"不做"的窗口必须一致。 */

/* ---------- 打开焦点程序所在文件夹（src/folder.c） ----------
   C + \ ：把"前台那个程序所在文件夹"用**系统的标准接口**打开，并选中该 exe 本身
   —— 等价于资源管理器右键的「打开文件所在的位置」。

   用的是 shell 的公开接口，**不拼 explorer.exe 命令行**：
     · `SHParseDisplayName(路径)`      路径 -> 绝对 PIDL（交给 shell 命名空间去解析）
     · `SHOpenFolderAndSelectItems(pidl, 0, NULL, 0)`
       官方文档写明：`cidl = 0` 时传"要选中的那一项的 PIDL"，函数会**打开它的父
       文件夹并选中它**。于是"开哪个窗口、要不要复用已开着的窗口"全由 shell 自己
       决定；系统换成别的文件管理器接管时这里也不用改。
   两条硬约束：① 调用线程必须先 `CoInitialize`，否则接口直接失败；
   ② 该调用是**同步阻塞**的（要等 explorer 响应），绝不能落在主线程 —— 所以它
   自己开一次性线程执行，理由见 folder.c 文件头。

   失败原因只用一个整数报回主线程（WM_FOLDER_OPEN）：气泡文案在主线程拼，
   工作线程不碰托盘状态。 */
#define FOLDER_OK           0   /* 已交给 shell：不弹气泡，只在记录模式下写日志           */
#define FOLDER_ERR_SHELLWIN 1   /* 前台是桌面/任务栏一类壳窗口：没有"所在文件夹"可开     */
#define FOLDER_ERR_NOPATH   2   /* 拿不到进程映像全路径（权限被拒 / 进程已退出 / UWP 解析不到） */
#define FOLDER_ERR_SHELL    3   /* shell 接口失败（路径解析不到，或打开请求被拒）         */
void FolderOpenRequest(HWND target);
                        /* 动作入口：把这件事丢给一次性线程后**立刻返回**（不占 act 队列） */
void FolderRevealPath(const WCHAR* path);
                        /* 同一个 shell 链路，但目标由**调用方给路径**而不是由窗口去查
                           （"打开某份文件所在的位置"用它：日志结束的通知被点击时就是这么用的）。
                           路径为空 / 文件不存在（没轮转过的话，记录期间刚被清空的日志文件
                           也可能不存在）时退回**所在目录**；连目录都解析不到就什么都不做。
                           同样是丢给一次性线程后立刻返回。 */
BOOL FolderProcessPathOf(HWND target, WCHAR* out, size_t cap, int* err);
                        /* 目标窗口 -> 进程映像全路径（含 UWP 的子窗口解析）。
                           同步、纯本地、不碰 shell —— 测试与实现共用（传本进程的窗口
                           即得本 exe 的路径）。失败返回 FALSE 且 *err = FOLDER_ERR_* */
BOOL FolderResolveTarget(const WCHAR* path, WCHAR* out, size_t cap
#ifdef CAP_TEST
                         , BOOL* dirOnly
#endif
                         );
                        /* 路径 -> shell 该打开/选中的那个对象（纯函数：不碰 shell、
                           不碰线程，所以可单测）。文件在 -> 原样返回（要选中它）；文件不在 ->
                           改成它的父目录（`SHOpenFolderAndSelectItems` 拿目录自己当"要选中的
                           项"时会去选中**上一级**里的那个目录，不是我们要的），目录则不需要
                           这个降级、只去掉多余的尾部分隔符（`C:\dir\` 解析不出 PIDL）。
                           **只给 `CAP_TEST` 编出来的断言用**的那个形参在下面说明。 */

/* ---------- 窗口置顶（src/pin.cpp，`Caps + T`） ----------
   行为与视觉对齐 PowerToys 的 AlwaysOnTop：把前台窗口钉在最上层（再按一次放开），
   被钉的窗口外面套一圈强调色圆角边框，钉住 / 放开各响一声系统音。
   全部实现（建窗、绘制、WinEvent 跟随、定时器、虚拟桌面判定、音效）都跑在
   pin.cpp 自己的线程上，这里只有三个投递入口与一个只读镜像 ——
   钩子宿主线程与动作线程都只发消息，不碰任何共享可变状态。

   ★ 目标窗口由**调用方**给定（动作线程用开火那一刻的前台窗口快照，见 main.c 的
     PostAct），不是本模块现读：投递是异步的，等它跑到时前台可能已经换了。 */
void PinInit(void);            /* WinMain：起置顶线程并等它就绪（失败则功能静默不可用） */
void PinShutdown(void);        /* 退出 / 重启：解除全部置顶后停线程 */
void PinToggle(HWND target);   /* 动作线程：toggle 一个窗口的置顶（非阻塞） */
void PinUnpinAll(void);        /* 托盘「取消全部置顶」：一次性全放（非阻塞） */
int  PinCount(void);           /* 当前钉住几个（主线程读镜像：托盘菜单据此置灰） */
BOOL PinFrameRect(const RECT* target, int thickness, RECT* out);
                        /* 纯函数：目标窗口外框 -> 边框窗矩形（四边各外扩 thickness）。
                           单测用它覆盖几何，不碰真实窗口。 */
int  PinCornerRadiusFor(int pref);
                        /* 纯函数：DWM 角偏好 -> 圆角半径（0 = 直角）。与 PowerToys 的
                           WindowCornerUtils 同表，单测覆盖。 */

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
void ActWheelPost(int delta, int mul, BOOL full);   /* 投递一次纵向放大（Caps+滚轮），非阻塞。
                                                       full=物理格已被钩子吞掉，mul 格全由我们注入 */
void ActWheelPostH(int delta, int n);    /* 投递一次横向滚动（Caps+Shift），注入 n 格 */
/* 松手时"陈旧"的判据：比这还旧的请求视觉上已经过期（用户早就滚过去了），
   留着补发只会变成松手之后的尾流；而刚滚的那几格必须补完 —— 它们就是用户
   期待的那次加速（实测每次甩轮尾部会被整队清掉 2~4 条）。 */
#define ACT_WHEEL_STALE_MS 120
/* 丢弃**陈旧**的未补发请求（松开 C / 引擎复位）：只丢队头超过 ms 的，刚滚的
   那几格照常补发 —— 整队清空会让每次甩轮的尾部少几格（"有时候不加速"的成因）。
   ms<=0 = 整队清空。 */
void ActWheelDropOlder(int ms);
void ActWheelClear(void);                /* 整队清空（通道停止/退出/暂停用） */
