/* ================= cap_nav：按词跳转 / 输入法 / 自动化接口去向 =================
   本文件**只被 cap.h 包含**。 */
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
