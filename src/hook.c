#include "cap.h"

/* 全局钩子句柄 */
HHOOK g_hook = NULL;
GlobalState g_state;

/* C（CapsLock）实时状态：钩子线程写，主线程与鼠标钩子读。
   修饰模式（增强/位移）的按住位图不在这里 —— 它在 keymap.c 的模式表里，
   由 ModeKeyEvent 维护、ModeDown/ModePending 查询。
   钩子从不判断"按的是 D 还是 Alt"，只问"这个键属于哪个模式"。 */
volatile BOOL g_cMod = FALSE;

/* 注：g_actEnh / g_actShf / g_btnCount 是和弦引擎状态，定义在 main.c。
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

/* 发一次动作。目标键与属性都来自键位表（src/keymap.c）：
   · 目标虚拟键用 ActOutVk —— 表里 outVk=0 的动作（词跳/诊断）不走这里；
   · 带 ACTF_NO_SHIFT 的动作（开关键）即便位移模式生效也裸发。
   shift 是**开火快照**里的位移模式，不是"此刻按着 F 吗"。 */
void SendActOnce(int act, BOOL shift) {
    int vk = ActOutVk(act);
    if (!vk) return;
    SendKeyOnce(vk, ActHasFlag(act, ACTF_NO_SHIFT) ? FALSE : shift);
}

/* 松手补齐：仅增强模式（快照 enh 为真）时补到 3 次；已发送次数由调用方
   （引擎）给出，本函数不回写任何全局状态。
   排除规则也来自键位表的标志，不再一个一个列举动作 id：
     · ACTF_WORDJUMP    —— 词跳不走按键发送，补齐无意义；
     · ACTF_SINGLE_SHOT —— 翻转动作补齐会把"切一次"变成"切三次"（净效果又回到
       原状态），这正是 caps+tab 时灵时不灵的成因之一。 */
void ReleaseFill(int act, BOOL enh, BOOL shift, int count) {
    if (!enh) return;
    if (count >= 3) return;
    if (ActHasFlag(act, ACTF_WORDJUMP))    return;
    if (ActHasFlag(act, ACTF_SINGLE_SHOT)) return;
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

        /* ---- 修饰模式的激活键（默认 增强=Alt/D、位移=F） ----
           哪些键属于哪个模式由 keymap.c 的模式表决定（可由配置改），这里只问
           "这个 vk 属于哪个模式" —— 所以"以后把 D 删掉、只留 Alt"不用动这里一行。

           **始终跟踪**按下/抬起（没按 C 时也跟踪）：否则"没按 C 的时候松开
           修饰键"会让按住位图残留，之后按 C 再随便一滚就被当成增强模式。
           仅在 C 按住期间屏蔽这些键；不按 C 时完全放行 —— D / F 照常打字。
           Alt 单独按下时系统发 WM_SYSKEYDOWN（上面的 down 判断已覆盖），
           屏蔽掉它就不会出现"Alt 被系统看到后激活菜单栏"的情况。 */
        int mode = ModeFindByVk(vk);
        if (mode >= 0) {
            BOOL was = ModeDown(mode);
            ModeKeyEvent(mode, vk, down);
            if (was && !ModeDown(mode))          /* 最后一个激活键也抬起了 */
                PostMessageW(g_mainWnd, WM_MODE_UP, (WPARAM)mode, 0);
            if (!g_cMod) return CallNextHookEx(g_hook, nCode, wParam, lParam);
            return 1;   /* C 按住期间：修饰键本身不外泄 */
        }

        /* ---- 未进入映射模式：放行 ---- */
        if (!g_cMod) return CallNextHookEx(g_hook, nCode, wParam, lParam);

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

/* ================= 鼠标钩子：按住 C 时滚轮加速 =================
   为什么用低级钩子而不是 Raw Input：**必须能认出"这个滚轮事件是我们自己
   注入的"**。Raw Input（RIDEV_INPUTSINK）拿不到"注入"标记，放大后的滚轮会
   再触发一次 WM_INPUT → 再放大 → 无限放大；MSLLHOOKSTRUCT.flags 的
   LLMHF_INJECTED 正是为这个场景准备的，低级钩子路线才能干净地过滤掉自己。

   代价是：这个回调落在**全局鼠标输入的关键路径**上（每一次鼠标移动都会
   进来一次），所以回调里只做几次比较然后 PostMessage，绝不在这里注入、
   也不做任何查询 —— 连 GetAsyncKeyState 都不能调（会明显加剧鼠标卡顿），
   "C 是否按住"直接读键盘钩子已经维护好的 g_cMod。
   注入放到主线程的消息处理里做（见 main.c 的 WM_WHEEL_ACCEL）。
   注：这也意味着主线程若被长时间占住会拖慢鼠标输入 —— 这正是"动作发送
   移到执行线程"（act.c）要解决的问题，两者是同一根因的两个面。 */
static HHOOK g_mouseHook = NULL;

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    /* 最快路径过滤：绝大多数事件（鼠标移动）在第一个条件就返回 */
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
        if (m && !(m->flags & LLMHF_INJECTED) && !g_state.paused && g_cMod) {
            int mul = ConfigGetWheelScrollMul();
            int delta = (int)(short)HIWORD(m->mouseData);   /* 高字是滚轮增量 */
            if (mul > 1 && delta != 0)
                PostMessageW(g_mainWnd, WM_WHEEL_ACCEL, (WPARAM)delta, (LPARAM)mul);
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

BOOL HookInstall(void) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                               GetModuleHandleW(NULL), 0);
    if (!g_hook) return FALSE;
    /* 滚轮加速的鼠标钩子装不上不影响主映射，降级即可（托盘会显示出来） */
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                    GetModuleHandleW(NULL), 0);
    return TRUE;
}

void HookUninstall(void) {
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
}

BOOL HookMouseAvailable(void) { return g_mouseHook != NULL; }