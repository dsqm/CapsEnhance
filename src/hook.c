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

/* 开火时的修饰快照（主线程开火时写入） */
volatile BOOL g_actD = FALSE;
volatile BOOL g_actF = FALSE;
volatile int  g_btnCount = 0;

/* 当前被按住的触发键 act（钩子线程去抖用） */
static volatile int g_downAct = ACT_NONE;

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

/* 发送按键：后半段按住 shift（Shift↓ 要点↓点↑… Shift↑），缺移位生效 */
static void SendClick(int vkcode, BOOL shift, int count) {
    WORD vd = (WORD)vkcode;
    WORD scd = (WORD)MapVirtualKeyW((UINT)vkcode, MAPVK_VK_TO_VSC);
    INPUT arr[1 + 2 * 8 + 1];   /* count<=8（本程序恒为 1），留足余量 */
    int k = 0;
    if (shift) {
        memset(&arr[k], 0, sizeof(INPUT));
        arr[k].type = INPUT_KEYBOARD;
        arr[k].ki.wVk = VK_LSHIFT;
        arr[k].ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
        k++;
    }
    for (int i = 0; i < count && i < 8; i++) {
        arr[k].type = INPUT_KEYBOARD;
        arr[k].ki.wVk = vd; arr[k].ki.wScan = scd;
        arr[k].ki.dwFlags = 0; k++;
        arr[k].type = INPUT_KEYBOARD;
        arr[k].ki.wVk = vd; arr[k].ki.wScan = scd;
        arr[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
    }
    if (shift) {
        memset(&arr[k], 0, sizeof(INPUT));
        arr[k].type = INPUT_KEYBOARD;
        arr[k].ki.wVk = VK_LSHIFT;
        arr[k].ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
        arr[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
    }
    SendInput(k, arr, sizeof(INPUT));
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

/* 发一次动作（用开火快照的修饰；词跳 N/M 由主线程 NavJump 处理，不走这里） */
void SendActOnce(int act) {
    int vk = ActToVk(act);
    if (!vk) return;
    BOOL shift = g_actF;
    if (act == ACT_CAPSLOCK) {
        SendClick(VK_CAPITAL, FALSE, 1);   /* 恢复真实 CapsLock 切换 */
        return;
    }
    SendClick(vk, shift, 1);
}

/* 松手补齐：仅增益模式 + 非词跳动作；若开火次数<3 补齐到3 */
void ReleaseFill(int act) {
    if (!g_actD) return;
    if (g_btnCount >= 3) { g_btnCount = 0; return; }
    if (act == ACT_WORD_L || act == ACT_WORD_R) { g_btnCount = 0; return; }
    while (g_btnCount < 3) { SendActOnce(act); g_btnCount++; }
    g_btnCount = 0;
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

        /* ---- C 键（CapsLock）自身 ---- */
        if (vk == CLI_CAPS) {
            if (down && !g_cMod) {
                g_cMod = TRUE;
                PostMessageW(g_mainWnd, WM_CAPS_DOWN, 0, 0);   /* 通知主线程复位 */
            } else if (!down && g_cMod) {
                g_cMod = FALSE;
                PostMessageW(g_mainWnd, WM_CAPS_UP, 0, 0);
            }
            return 1;   /* 全程屏蔽，无效果 */
        }

        /* ---- 未进入映射模式：放行 ---- */
        if (!g_cMod) return CallNextHookEx(g_hook, nCode, wParam, lParam);

        /* ---- C 按住期间：D/F 修饰 ---- */
        if (vk == 'D') { g_dMod = down; g_pendD = down; return 1; }
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
            if (down) {
                if (g_downAct != act) {          /* 去抖：忽略系统自动重复 */
                    g_downAct = act;
                    PostMessageW(g_mainWnd, WM_TRIG_DOWN, (WPARAM)act, 0);
                }
            } else {
                if (g_downAct == act) {
                    g_downAct = ACT_NONE;
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