#include "cap.h"

/* 全局钩子句柄 */
HHOOK g_hook = NULL;
GlobalState g_state;

/* 修饰键实时状态（钩子线程写，主线程读） */
volatile BOOL g_cMod = FALSE;
volatile BOOL g_dMod = FALSE;
volatile BOOL g_fMod = FALSE;

/* 本次和弦 D/F 门闩（钩子线程维护，随物理按键立即更新） */
volatile BOOL g_pendD = FALSE;
volatile BOOL g_pendF = FALSE;

/* 注：g_actD / g_actF / g_btnCount 是和弦引擎状态，定义在 main.c。
   实际发送跑在 act.c 的工作线程上，快照随 ActJob 传递，这里不再持有。 */

/* 物理按键状态表：只记录真实（非注入）按键的按下/抬起。
   GetAsyncKeyState 反映的是 OS 的"逻辑"状态——被注入的键同样会置位，
   因此它区分不了"用户真的按着"和"键卡住了"。必须由钩子自己维护物理状态，
   这正是 AHK 安装键盘钩子后能分辨 GetKeyState(,"P") 物理状态的原因。 */
static volatile BYTE g_phys[256];
BOOL PhysKeyDown(int vk) {
    if (vk < 0 || vk > 255) return FALSE;
    return g_phys[vk] != 0;
}

/* 装好钩子后调用：把"此刻逻辑按下"的键视为物理按住。
   否则用户在程序启动之前就按住的键（钩子没见过它的按下）会在清理时
   被误判成卡键而清掉。 */
void PhysSeedFromLogical(void) {
    static const int vks[] = {
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
        VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN, VK_CAPITAL
    };
    for (int i = 0; i < (int)(sizeof(vks) / sizeof(vks[0])); i++)
        if (GetAsyncKeyState(vks[i]) & 0x8000) g_phys[vks[i]] = 1;
}

/* 同时按住的触发键集合（每个动作一个 bit）。
   原来只用一个 g_downAct：J 按住时再按 K 会把 J 顶掉，于是 J 的抬起
   再也发不出去（g_downAct 已不是 J），引擎收不到 UP 就永远重复 J —— 
   "按住 Caps 同时按 J 和 K 会一直输出左键"就是这么来的。 */
static volatile DWORD g_downMask = 0;

/* 映射表：虚拟键码 -> 动作 */
typedef struct { int vk; int act; } KeyMap;
static const KeyMap kMap[] = {
    { 'J',    ACT_LEFT       },
    { 'K',    ACT_DOWN       },
    { 'L',    ACT_RIGHT      },
    { 'I',    ACT_UP         },
    { 'P',    ACT_HOME       },
    { VK_OEM_1, ACT_END      },        /* ; */
    { 'R',    ACT_BACKSPACE  },
    { 'W',    ACT_DELETE     },
    { VK_SPACE,   ACT_ENTER  },
    { VK_TAB,     ACT_CAPSLOCK },
    { 'N',    ACT_WORD_L     },
    { 'M',    ACT_WORD_R     },
};
#define KMAP_N (sizeof(kMap)/sizeof(kMap[0]))

static int ActOfVk(int vk) {
    for (int i = 0; i < (int)KMAP_N; i++)
        if (kMap[i].vk == vk) return kMap[i].act;
    return ACT_NONE;
}

/* act -> 目标虚拟键 */
static int ActToVk(int act) {
    switch (act) {
        case ACT_LEFT:   return VK_LEFT;
        case ACT_UP:     return VK_UP;
        case ACT_RIGHT:  return VK_RIGHT;
        case ACT_DOWN:   return VK_DOWN;
        case ACT_HOME:   return VK_HOME;
        case ACT_END:    return VK_END;
        case ACT_BACKSPACE: return VK_BACK;
        case ACT_DELETE: return VK_DELETE;
        case ACT_ENTER:  return VK_RETURN;
        case ACT_CAPSLOCK: return VK_CAPITAL;
        default: return 0;
    }
}

/* 发一次动作。shift = 开火快照里的 F（位移）；词跳 N/M 由 NavJump 处理，
   不走这里。真实 CapsLock 切换不需要带 Shift。 */
void SendActOnce(int act, BOOL shift) {
    int vk = ActToVk(act);
    if (!vk) return;
    SendKeyOnce(vk, (act == ACT_CAPSLOCK) ? FALSE : shift);
}

/* 松手补齐：仅增益模式（快照 d 为真）+ 非词跳动作；已发送次数 <3 则补到 3。
   次数由调用方（引擎）给出，本函数不回写任何全局状态。 */
void ReleaseFill(int act, BOOL d, BOOL shift, int count) {
    if (!d) return;
    if (count >= 3) return;
    if (act == ACT_WORD_L || act == ACT_WORD_R) return;
    while (count < 3) { SendActOnce(act, shift); count++; }
}

/* 是否软件注入的按键（避免自己被自己触发） */
static BOOL IsInjected(const KBDLLHOOKSTRUCT *p) {
    return (p->flags & LLKHF_INJECTED) != 0;
}

/* 底层键盘钩子过程 */
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT*)lParam;
        if (IsInjected(p)) return CallNextHookEx(g_hook, nCode, wParam, lParam);
        if (g_state.paused) return CallNextHookEx(g_hook, nCode, wParam, lParam);

        BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        int  vk   = (int)p->vkCode;

        /* 物理状态：注入的键在上面已放行，这里记录的都是真实按键 */
        if (vk >= 0 && vk < 256) g_phys[vk] = (BYTE)(down ? 1 : 0);

        /* ---- C 键（CapsLock）自身 ---- */
        if (vk == CLI_CAPS) {
            if (down && !g_cMod) {
                g_cMod = TRUE;
                g_downMask = 0;
                PostMessageW(g_mainWnd, WM_CAPS_DOWN, 0, 0);   /* 通知主线程复位 */
            } else if (!down && g_cMod) {
                g_cMod = FALSE;
                g_downMask = 0;
                SendArrowsRequestInterrupt();   /* C 松开：立即打断逐格输出 */
                PostMessageW(g_mainWnd, WM_CAPS_UP, 0, 0);
            }
            return 1;   /* 全程屏蔽，无效果 */
        }

        /* ---- 未进入映射模式：放行 ---- */
        if (!g_cMod) return CallNextHookEx(g_hook, nCode, wParam, lParam);

        /* ---- C 按住期间：D/F 修饰 ----
           左 Alt 等同 D（增益）：Caps+Alt+… 与 Caps+D+… 完全一样。
           Alt 单独按下时系统发 WM_SYSKEYDOWN（上面的 down 判断已覆盖），
           且这里全程屏蔽，不会出现"Alt 被系统看到后激活菜单栏"的情况。 */
        if (vk == 'D' || vk == VK_LMENU) { g_dMod = down; g_pendD = down; return 1; }
        if (vk == 'F') { g_fMod = down; g_pendF = down; return 1; }

        /* ---- C 按住期间：` 键触发焦点探测（仅按下沿） ---- */
        if (vk == PROBE_KEY) {
            static volatile BOOL armed = FALSE;
            if (down && !armed) { armed = TRUE; PostMessageW(g_mainWnd, WM_PROBE, 0, 0); }
            else if (!down) armed = FALSE;
            return 1;
        }

        /* ---- 触发键 ---- */
        int act = ActOfVk(vk);
        if (act != ACT_NONE) {
            DWORD bit = (DWORD)1 << act;
            if (down) {
                if (!(g_downMask & bit)) {       /* 去抖：忽略系统自动重复 */
                    g_downMask |= bit;
                    /* 正在逐格输出且按下了不同触发键（改道）：立即打断 */
                    if (SendArrowsConflicts(act))
                        SendArrowsRequestInterrupt();
                    PostMessageW(g_mainWnd, WM_TRIG_DOWN, (WPARAM)act, 0);
                }
            } else {
                if (g_downMask & bit) {          /* 每个键都能拿到自己的抬起 */
                    g_downMask &= ~bit;
                    PostMessageW(g_mainWnd, WM_TRIG_UP, (WPARAM)act, 0);
                }
            }
            return 1;
        }

        return 1;  /* C 按住期间其它普通键：屏蔽 */
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

BOOL HookInstall(void) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                               GetModuleHandleW(NULL), 0);
    return g_hook != NULL;
}

void HookUninstall(void) {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
}