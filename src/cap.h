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
#define WM_PROBE       (WM_APP + 6) /* 探测焦点控件（钩子线程投递） */

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

/* 长按重复节奏 */
#define REPEAT_FIRST_DELAY   150     /* ms：开火到第一次重复                 */
#define REPEAT_INTERVAL      35      /* ms：后续重复间隔                      */
#define CHORD_SETTLE         50      /* ms：和弦稳定期，等 D/F 修饰到位      */

/* ---------- 全局共享状态（钩子线程写，主线程读） ---------- */
typedef struct {
    volatile LONG paused;      /* 1=暂停映射（CapsLock 恢复正常）         */
} GlobalState;

extern GlobalState g_state;

/* 诊断记录开关：C+` 触发开始/结束（主线程读写） */
extern volatile BOOL g_recording;

/* 修饰键实时状态与"开火快照"（volatile，跨线程） */
extern volatile BOOL g_cMod;    /* C 按住 */
extern volatile BOOL g_dMod;    /* 增益 D 按住（实时） */
extern volatile BOOL g_fMod;    /* 位移 F 按住（实时） */
extern volatile BOOL g_pendD;   /* 本次和弦 D 门闩（钩子线程维护，物理顺序，免疫派发竞态） */
extern volatile BOOL g_pendF;   /* 本次和弦 F 门闩 */
extern volatile BOOL g_actD;    /* 本次动作开火时的 D 快照 */
extern volatile BOOL g_actF;    /* 本次动作开火时的 F 快照 */
extern volatile int  g_btnCount;/* 本次动作已发送次数 */

/* ---------- 钩子模块 ---------- */
extern HHOOK g_hook;
BOOL HookInstall(void);
void HookUninstall(void);

/* ---------- 发送/补齐 ---------- */
void SendActOnce(int act);       /* 发一次动作（用开火快照修饰） */
void ReleaseFill(int act);       /* 松手补齐：增益模式点按 -> 补到3 */

/* ---------- 按键发送模块（src/send.c） ----------
   所有模拟按键集中在这里：扩展键标志、逐条发送节奏、Shift/Ctrl 组合。
   之前 hook.c(SendClick) 与 nav.c(SendArrows/兜底) 各写一份，两边的
   缺陷也不一样（缺 KEYEVENTF_EXTENDEDKEY / Ctrl 提前抬起），统一后只修一处。 */
void SendKeyOnce(int vk, BOOL shift);        /* 单击一个键，shift 全程按住 */
void SendArrows(int dir, int n, BOOL shift); /* n 个左/右箭头，shift 全程按住 */
void SendNativeWordJump(int dir, BOOL shift);/* 原生 Ctrl(+Shift)+方向：词跳兜底 */
void SendClearStuckModifiers(void);          /* 补齐修饰键"抬起"，清卡键（重启/退出用） */

/* ---------- 主窗口 ---------- */
extern HWND g_mainWnd;

/* ---------- 诊断/日志 ---------- */
void CapLog(const char* fmt, ...);   /* 追加写 CapsEnhance.log（可多线程调用） */
void NavProbe(void);                 /* 探测当前焦点控件并写入日志 + 弹窗 */
void NavBaseInfo(void);              /* 启动时采集系统/环境基线（不落盘，C+` 诊断时输出） */

/* ---------- 词跳模块 ---------- */
int NavJump(int dir, BOOL shift);      /* 前台控件 + dir + shift(位移=F) -> 移动；返回移动格数(<0失败) */

#endif