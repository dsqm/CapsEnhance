#include "cap.h"

/* 全局钩子句柄 */
HHOOK g_hook = NULL;
GlobalState g_state;

/* C（CapsLock）实时状态：钩子线程写，主线程与鼠标钩子读。
   修饰模式（增强/选区）的按住位图不在这里 —— 它在 keymap.c 的模式表里，
   由 ModeKeyEvent 维护、ModeDown 查询（唯一判据，见 cap.h）。
   钩子从不判断"按的是 D 还是 Alt"，只问"这个键属于哪个模式"。 */
volatile BOOL g_cMod = FALSE;

/* 真实 Shift 的按住状态：钩子线程写，鼠标钩子读。
   用途只有一个 —— **Caps + Shift + 滚轮 = 横向滚动**。
   注意与 keymap.c 的"选区模式"（默认 F，作用是给动作加 Shift）不是一回事：
   那是"模拟 Shift 的效果"，这里读的是**用户真的按着 Shift**。
   为什么不在鼠标钩子里直接调 GetAsyncKeyState：那个回调在全局鼠标输入的关键
   路径上（每次鼠标移动都进来），任何系统查询都会明显加剧鼠标卡顿 —— 所以状态
   由键盘钩子维护、鼠标钩子只读几个 volatile 变量（与 g_cMod 同一套做法）。 */
volatile BOOL g_shiftMod = FALSE;
static volatile DWORD s_shiftMask = 0;   /* bit0=左 Shift, bit1=右 Shift */

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
    /* Shift 位图同步种子化：否则"启动前就按着 Shift"时 Caps+滚轮不会横向滚动 */
    s_shiftMask = 0;
    if (g_phys[VK_LSHIFT]) s_shiftMask |= 1u;
    if (g_phys[VK_RSHIFT]) s_shiftMask |= 2u;
    g_shiftMod = (s_shiftMask != 0);
}

/* 同时按住的触发键集合（每个动作一个 bit）。
   原来只用一个 g_downAct：J 按住时再按 K 会把 J 顶掉，于是 J 的抬起
   再也发不出去（g_downAct 已不是 J），引擎收不到 UP 就永远重复 J —— 
   "按住 Caps 同时按 J 和 K 会一直输出左键"就是这么来的。 */
static volatile DWORD g_downMask = 0;

/* `（探测键）的按下沿闩锁：**必须是文件作用域** ——
   它原来写成一个 `static volatile BOOL armed` 的函数内静态变量，于是复位路径
   （ModHoldReset）够不到它，而"松 C 之前先松 `" 的场景下，那个 keyup 会走
   `if (!g_cMod) 放行` 分支直接出去，闩锁永远保持 TRUE：下一次 C+` 的首次按下
   不触发探测（"有时要按两下"）。复位时必须能清它，所以提到文件作用域。 */
static volatile BOOL g_probeArmed = FALSE;

/* 本次 CapsLock 是否"决定不介入"（反着按：见 ChordKeyHeldBeforeCaps）。
   down 时决定一次，up 沿用 —— 与 g_cMod 一样是"一次会话一份决定"。 */
static volatile BOOL g_capsSkip = FALSE;

/* ---------- "本会话内被吞过 down"的按键记账（Alt 卡键的根因） ----------
   一个键的 keydown 与 keyup 必须**同进同出**：要么都放行、要么都吞掉。
   一吞一放会让系统侧状态错位，而危险方向是"down 放行、up 被吞" ——
   系统此后一直认为该键按着，用户看到的就是"卡键"。

   实测（用户报告）：**先按 Alt，再按 Caps，然后先松开 Alt** ——
     · Alt down 时 Caps 还没按（g_cMod 为假）→ 走放行分支，系统看到了 Alt 按下；
     · 之后 Caps 按住期间松开 Alt → 那次 keyup 被"C 按住期间修饰键不外泄"
       吞掉，而往后没有任何人补发 Alt 的抬起 → 目标程序里 Alt 永久按下。
   反过来的顺序（down 吞、up 放行）不会卡键，但会把一个孤立的 keyup 漏给
   目标程序 —— "C 按住期间的键不外泄"这个承诺就破了。

   规则：**up 沿用 down 的决定**。down 被吞的键登记在此表，其 up 无论此刻
   Caps 是否还按着，都照吞（并出表）。于是不论松手顺序如何，两侧永远配对。

   表容量 32：登记的是"被我们吞过 down"的键，上界是 8 个修饰键 + 13 个触发键
   + 1 个探测键（末尾兜底只登记修饰键，普通字母键量大且其孤立 up 无害，
   不占表）。只在钩子线程访问，无需加锁。 */
#define SWALLOW_MAX 32
static volatile int g_swallowVk[SWALLOW_MAX];
static volatile int g_swallowN = 0;

static BOOL IsModifierVk(int vk) {
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT:
    case VK_LCONTROL: case VK_RCONTROL:
    case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return TRUE;
    default:
        return FALSE;
    }
}
/* down 被吞 -> 登记，等它的 up 来配对 */
static void SwallowNote(int vk) {
    for (int i = 0; i < g_swallowN; i++) if (g_swallowVk[i] == vk) return;
    if (g_swallowN < SWALLOW_MAX) g_swallowVk[g_swallowN++] = vk;
}
/* up 来了 -> 若 down 被我们吞过，返回 TRUE（这次 up 也必须吞）并出表 */
static BOOL SwallowTake(int vk) {
    for (int i = 0; i < g_swallowN; i++) {
        if (g_swallowVk[i] != vk) continue;
        for (int j = i; j + 1 < g_swallowN; j++) g_swallowVk[j] = g_swallowVk[j + 1];
        g_swallowN--;
        return TRUE;
    }
    return FALSE;
}

/* "放行"的统一出口：若这个键的 down 曾被我们吞掉，那这次 up 也必须吞。
   **必须只在放行点使用** —— 记账绝不能挡在内部处理之前，否则"吞掉 up"
   会连带把 WM_TRIG_UP 之类的内部投递一起吃掉（实测：引擎再也收不到抬起，
   短点不开火、长按停不下来）。内部处理与"是否放行给系统"是两件事。 */
#define PASS_OR_SWALLOW(vk, down) \
    (((!(down)) && SwallowTake(vk)) ? 1 : CallNextHookEx(g_hook, nCode, wParam, lParam))

/* ---------- Caps 必须是本次和弦的**第一个**键 ----------
   用户明确要求：反着按的组合（先按 Alt 之类、再按 Caps）本程序**完全不处理**，
   只拦截"Caps 开头"的和弦。

   这不只是偏好，也是上面那个卡键的必要防线：Caps 之前若已有键被系统看到
   （down 已放行），我们随后开始吞它的 up 就必然不对称。与其逐键补救，
   不如**整条会话都不介入** —— Caps 与相关键全按系统原生行为走，天然对称。

   判据用 g_phys（钩子自己维护的**物理**按下表，不含注入），覆盖三类键：
     · 修饰键（Alt/Ctrl/Shift/Win 左右）—— 系统的组合键语义会被我们打断；
     · 触发键（J K L I P ; R W Space Tab N M）与模式激活键（默认 Alt/D/F）；
     · 诊断键 `。
   键位表是数据的，所以这里一律查询（ActOfVk / ModeFindByVk），不写死键名；
   以后改键位不用回来改这里。

   与"系统是否也认为它按着"交叉校验（SysKeyHeld），两个作用：
     ① 只对有物理按下的键查（通常 0~2 次；Caps 按下又是极低频事件），
        不构成钩子路径上的负担；
     ② **修正残留**：物理表说按着、系统并不认为按着 —— 说明我们漏收过某个
        抬起事件（例如锁屏时松开了键）。这种残留若不管，往后每一次 Caps
        都会被跳过（表现为 Caps 突然完全失效、CapsLock 也不切换），所以顺手
        清掉物理位。

   SysKeyHeld 走一层可替换的函数指针：单元测试是**直接调用**这个钩子过程的，
   系统并不知道那些伪造事件，所以测试会把它替换成自己的状态表，从而能真实
   地验证"反着按不介入"与"吞放必须成对"这两条。 */
static BOOL (*g_sysKeyHeld)(int vk) = NULL;
static BOOL SysKeyHeld(int vk) {
    if (g_sysKeyHeld) return g_sysKeyHeld(vk);
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static BOOL ChordKeyHeldBeforeCaps(void) {
    for (int vk = 1; vk < 256; vk++) {
        if (vk == CLI_CAPS || !g_phys[vk]) continue;
        if (!SysKeyHeld(vk)) { g_phys[vk] = 0; continue; }
        if (IsModifierVk(vk) || ActOfVk(vk) != ACT_NONE ||
            ModeFindByVk(vk) >= 0 || vk == PROBE_KEY)
            return TRUE;
    }
    return FALSE;
}

/* 清"按住状态"。暂停/重启时会漏掉抬起事件（钩子被跳过），留着就会粘住 ——
   比如暂停期间松开 Shift，恢复后 g_shiftMod 还是 TRUE，Caps+滚轮会莫名变成
   横向滚动；` 的按下沿闩锁、吞放记账同理。所以复位时主动清掉。 */
void ModHoldReset(void) {
    g_cMod = FALSE;
    s_shiftMask = 0;
    g_shiftMod = FALSE;
    g_downMask = 0;
    g_probeArmed = FALSE;
    g_capsSkip = FALSE;
    g_swallowN = 0;
    ModeResetAll();
}

/* 发一次动作。目标键与属性都来自键位表（src/keymap.c）：
   · 目标虚拟键用 ActOutVk —— 表里 outVk=0 的动作（按词跳转/诊断）不走这里；
   · 带 ACTF_NO_SHIFT 的动作（开关键）即便选区模式生效也裸发。
   shift 是**开火快照**里的选区模式，不是"此刻按着 F 吗"。 */
void SendActOnce(int act, BOOL shift) {
    int vk = ActOutVk(act);
    if (!vk) return;
    SendKeyOnce(vk, ActHasFlag(act, ACTF_NO_SHIFT) ? FALSE : shift);
}

/* 松手补齐：仅增强模式（快照 enh 为真）时补到 3 次；已发送次数由调用方
   （引擎）给出，本函数不回写任何全局状态。
   排除规则也来自键位表的标志，不再一个一个列举动作 id：
     · ACTF_WORDJUMP    —— 按词跳转不走按键发送，补齐无意义；
     · ACTF_SINGLE_SHOT —— 翻转动作补齐会把"切一次"变成"切三次"（净效果又回到
       原状态），这正是 caps+tab 时灵时不灵的成因之一。 */
void ReleaseFill(int act, BOOL enh, BOOL shift, int count) {
    if (!enh) return;
    if (count >= 3) return;
    if (ActHasFlag(act, ACTF_WORDJUMP))    return;
    if (ActHasFlag(act, ACTF_SINGLE_SHOT)) return;
    while (count < 3) { SendActOnce(act, shift); count++; }
}

/* ---------- 触发键抬起时随消息带走"这一刻"的修饰快照 ----------
   **只用于诊断日志**：引擎在按下那一刻就开火并冻结修饰快照了（见 main.c 的
   SnapMods），抬起时的处理只做"停重复 + 按增强模式补齐"，不再读修饰。
   留着它是为了让日志能显示"抬起时修饰还在不在"（排查点按补齐是否按预期
   触发时有用），所以它必须取**事件发生那一刻**的值：钩子回调与主线程的
   消息处理是异步的，用户在钩子里"点按 J"之后立刻松开 Alt 时，Alt 的抬起
   已经在钩子里清掉了按住位图，主线程现读会读到"Alt 已松"。 */
static LPARAM ModeSnapFlags(void) {
    LPARAM f = 0;
    if (ModeDown(MODE_ENHANCE)) f |= TRIGF_ENH;
    if (ModeDown(MODE_SHIFT))   f |= TRIGF_SHF;
    return f;
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

        BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        int  vk   = (int)p->vkCode;

        /* ---- "按任意键打断持续滚动"（2026-09-11 第二版） ----
           松手不再终止（滚起来就能松手读），所以终止条件改由"用户产生新输入"
           承担。位置很关键：放在 paused 判断与所有吞键分支**之前** —— 否则某些
           路径（暂停、反着按、C 会话内）永远走不到，就会出现"停不下来的滚动"。
           注入按键在上面已经被放行，所以我们自己的注入不会打断自己。

           ★ 只有**新按下**才算新输入：`g_phys` 在这里还是按下之前的快照（更新在
           下面），已经按着的键再来的 keydown 就是系统自动重复（typematic）。
           这一条是必需的，不是防抖 —— 按住 Caps/Alt 滚动时，被按住的键会持续
           重复，把重复也当成"新输入"，症状就是**滚一下就停**（第一格刚出去就被
           自己按住的那个键的重复打断）。是用户上报症状后的头号怀疑对象，但
           尚未在真机上逐事件坐实 —— 所以 act.c 的终止日志把 reason 与 vk 都写下来，
           下次出现可直接对上。（CapsLock 本身不重复：toggle 键；修饰键与普通键会。）
           没在滚动时它只是两次内存读，键盘热路径上可以接受。 */
        if (down && !PhysKeyDown(vk))
            ActAutoScrollInterrupt("按下按键", vk);

        if (g_state.paused) {
            /* 暂停期间完全不处理（连物理表都不更新）—— 但"暂停之前被我们
               吞过 down"的键，它的 up 仍要吞，否则又是"吞 down / 放 up"的
               不对称。这两条早退路径没有内部处理，记账放在这里最安全。 */
            if (!down && SwallowTake(vk)) return 1;
            return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }

        /* 物理状态：注入的键在上面已放行，这里记录的都是真实按键 */
        if (vk >= 0 && vk < 256) g_phys[vk] = (BYTE)(down ? 1 : 0);

        /* ---- 真实 Shift 跟踪（无条件，未按 C 时也要跟） ----
           只在按下/抬起时更新一个位图；左右各一位，免得"两个 Shift 只松一个"
           被误判成全部抬起。放在最前面是因为鼠标钩子要用它判断横向滚动，
           而那时可能已经过了 `if (!g_cMod)` 的放行分支。 */
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) {
            DWORD bit = (vk == VK_LSHIFT) ? 1u : 2u;
            if (down) s_shiftMask |= bit; else s_shiftMask &= ~bit;
            g_shiftMod = (s_shiftMask != 0);
        }

        /* ---- C 键（CapsLock）自身 ----
           只有"Caps 是本次和弦第一个按下的键"时才介入；否则整条会话都不处理
           （反着按，见 ChordKeyHeldBeforeCaps）。介入与否在 down 时决定一次，
           up 沿用同一个决定（g_capsSkip）—— 两侧必须对称，否则不是漏了
           CapsLock 的切换，就是把它吞掉变成完全没反应。 */
        if (vk == CLI_CAPS) {
            if (down) {
                /* CapsLock 是 toggle 键，系统不对它发自动重复，所以每次
                   WM_KEYDOWN 都是"新按下"。顺手清掉上一次会话可能残留的
                   标记：万一上一次的 keyup 丢了（钩子被暂停/摘掉），这里
                   能自愈，而不会因为残留标记把 Caps 永久吞掉。
                   跳过分支不发任何消息 —— "不介入"就该完全不打扰，
                   连引擎状态都不碰（可能正有别的动作在跑）。 */
                g_cMod = FALSE;
                g_capsSkip = FALSE;
                if (ChordKeyHeldBeforeCaps()) {
                    g_capsSkip = TRUE;      /* 本次会话完全不介入 */
                    if (g_recording.load())
                        CapLog("  Caps 不是和弦的第一个键（已有映射相关键按住），"
                               "本次不介入，CapsLock 保持原功能");
                    return CallNextHookEx(g_hook, nCode, wParam, lParam);
                }
                g_cMod = TRUE;
                g_downMask = 0;
                g_probeArmed = FALSE;   /* 新一轮和弦：探测键从"未按过"开始 */
                PostMessageW(g_mainWnd, WM_CAPS_DOWN, 0, 0);   /* 通知主线程复位 */
                return 1;
            }
            if (g_cMod) {
                g_cMod = FALSE;
                g_downMask = 0;
                /* 必须先清探测闩锁：若用户"按着 ` 直接松开 C"，` 的 keyup
                   会在上面 `if (!g_cMod) 放行` 分支出去，没人再清闩锁 ——
                   下一次 C+` 的首次按下就哑了。 */
                g_probeArmed = FALSE;
                SendArrowsRequestInterrupt();   /* C 松开：立即打断逐格输出 */
                PostMessageW(g_mainWnd, WM_CAPS_UP, 0, 0);
                return 1;
            }
            if (g_capsSkip) {           /* 反着按的那次会话：抬起来也放行 */
                g_capsSkip = FALSE;
                return CallNextHookEx(g_hook, nCode, wParam, lParam);
            }
            return 1;
        }

        /* ---- 修饰模式的激活键（默认 增强=Alt/D、选区=F） ----
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
            /* 记录模式下把模式键自己的按下/抬起也打出来：排查"没按 F 却带选区"
               （= 按住位图残留）时，这是唯一能证明"F 到底按没按"的证据 —— 否则
               只能从触发键的 enh/shf 字段反推，而那一层看到的正是被怀疑的东西。 */
            if (g_cMod && g_recording.load())
                CapLog("  [模式] %s vk=0x%02X %s -> 生效=%d",
                       (mode == MODE_ENHANCE) ? "增强(Alt/D)" : "选区(F)",
                       vk, down ? "按下" : "抬起", (int)ModeDown(mode));
            if (!g_cMod) return PASS_OR_SWALLOW(vk, down);
            if (down) SwallowNote(vk);           /* 记下：它的 up 也要吞 */
            else      SwallowTake(vk);           /* 这次 up 已吞，出表 */
            return 1;   /* C 按住期间：修饰键本身不外泄 */
        }

        /* ---- 未进入映射模式：放行 ---- */
        if (!g_cMod) return PASS_OR_SWALLOW(vk, down);

        /* ---- C 按住期间：` 键触发焦点探测（仅按下沿） ---- */
        if (vk == PROBE_KEY) {
            if (down) SwallowNote(vk);
            else      SwallowTake(vk);           /* 这次 up 已吞，出表 */
            if (down && !g_probeArmed) {
                g_probeArmed = TRUE;
                PostMessageW(g_mainWnd, WM_PROBE, 0, 0);
            } else if (!down) {
                g_probeArmed = FALSE;
            }
            return 1;
        }

        /* ---- 触发键 ---- */
        int act = ActOfVk(vk);
        if (act != ACT_NONE) {
            DWORD bit = (DWORD)1 << act;
            if (down) {
                SwallowNote(vk);
                if (!(g_downMask & bit)) {       /* 去抖：忽略系统自动重复 */
                    g_downMask |= bit;
                    /* 正在逐格输出且按下了不同触发键（改道）：立即打断 */
                    if (SendArrowsConflicts(act))
                        SendArrowsRequestInterrupt();
                    /* 修饰快照随消息带走（= **按下那一刻**的状态）。引擎在
                       处理这条消息时就开火，不能再现读 ModeDown：钩子回调与
                       主线程是异步的，"点按 J 后立刻松 Alt"时按住位图早已在
                       钩子里被清掉，现读会读到"Alt 已松"，点按补齐于是
                       时有时无。 */
                    PostMessageW(g_mainWnd, WM_TRIG_DOWN, (WPARAM)act,
                                 ModeSnapFlags());
                }
            } else {
                SwallowTake(vk);                 /* 这次 up 已吞，出表 */
                if (g_downMask & bit) {          /* 每个键都能拿到自己的抬起 */
                    g_downMask &= ~bit;
                    PostMessageW(g_mainWnd, WM_TRIG_UP, (WPARAM)act,
                                 ModeSnapFlags());   /* 事件时刻的修饰快照 */
                }
            }
            return 1;
        }

        /* C 按住期间其它普通键：屏蔽。
           修饰键（不在模式表里的真实 Ctrl/Shift/Win 等）要**记账** —— 它的
           down 被我们吞了，up 也必须吞（见 g_swallowVk）。普通字母键量大且
           遗孤的 up 无害，不占表。 */
        if (down) { if (IsModifierVk(vk)) SwallowNote(vk); }
        else      SwallowTake(vk);
        return 1;
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* ================= 鼠标钩子：按住 C 时的滚轮 =================
   三种语义，按 Shift > 增强修饰 > 无修饰 的优先级判定：

     · Caps+Shift（真实 Shift）：**横向滚动** —— 换轴向，物理格**吞掉**
       （放行就是"横一下、纵一下"的混滚，见下面 SendWheelHNotches 的说明）；
     · Caps + 增强修饰（默认 Alt/D）：**持续滚动** —— 滚一下就自己一直滚；
       物理格也**吞掉**：这时滚轮是"控制杆"而不是滚动器 —— 同向滚一下的
       唯一语义是**提速**，不该再顺带真滚一格（那就成了"提速 + 多滚一格"的
       双重效果）。启动时吞掉也不会没反馈：工作线程一激活就**立刻补发第一格**
       （见 act.c 的 AutoScrollThread），所以即时反馈来自我们自己的注入。
     · Caps（无修饰）：**倍数放大** —— 每格补发 (N-1) 格（N = WheelScrollMul）。
       物理格**放行**：它是"加强"而不是"替换"，靠的就是"系统滚一格 + 我们补几格"。

   为什么 Shift 排最前：它显式指定了"轴向"，比"滚多快"更具体。

   为什么用低级钩子而不是 Raw Input：**必须能认出"这个滚轮事件是我们自己
   注入的"**。Raw Input（RIDEV_INPUTSINK）拿不到"注入"标记，放大后的滚轮会
   再触发一次 WM_INPUT → 再放大 → 无限放大；MSLLHOOKSTRUCT.flags 的
   LLMHF_INJECTED 正是为这个场景准备的，低级钩子路线才能干净地过滤掉自己。

   代价是：这个回调落在**全局鼠标输入的关键路径**上（每一次鼠标移动都会
   进来一次），所以回调里只做几次比较然后 PostMessage，绝不在这里注入、
   也不做任何系统查询 —— 连 GetAsyncKeyState 都不能调（会明显加剧鼠标卡顿）。
   "C 是否按住"直接读键盘钩子已经维护好的 g_cMod；"是否增强模式"读
   ModeDown（就是把位图与零比一下，几次内存访问，不是系统调用）。
   注入一律在别的线程做（main.c 只投递，act.c 的专用通道执行）。
   注：这也意味着主线程若被长时间占住会拖慢鼠标输入 —— 这正是"动作发送
   移到执行线程"（act.c）要解决的问题，两者是同一根因的两个面。 */
static HHOOK g_mouseHook = NULL;

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
    /* 自己注入的事件一律放行：既不能让它们再触发一次逻辑，也不能让它们
       "打断持续滚动"（放大/持续滚动都在注入滚轮）。 */
    if (!m || (m->flags & LLMHF_INJECTED))
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    /* 移动是这里最热的事件（每次移动都进来）：第一个分支就放行，别为它多花比较。
       它也算"鼠标动作"，但**不算按键**，不打断持续滚动 —— 否则手一抖就停了。 */
    if (wParam == WM_MOUSEMOVE)
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

    if (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL) {
        BOOL vert  = (wParam == WM_MOUSEWHEEL);
        int  delta = (int)(short)HIWORD(m->mouseData);   /* 高字是滚轮增量 */
        BOOL chord = (!g_state.paused && g_cMod);        /* 和弦（Caps）还按着吗 */
        /* 横向接管的判据：只有**纵向轴**才谈得上"Shift 把它改成横向"（横向轴本来
           就是横向，不需要再翻一次）。倍数 0 = 该组合已被配置关闭 -> 不接管，
           让它退回下面的纵向语义（旧承诺）。 */
        int  hmul = (chord && vert && g_shiftMod) ? ConfigGetWheelHScrollMul() : 0;
        BOOL shiftToH = (hmul > 0);

        /* 记录模式下的现场：**只记物理事件**（注入的在上面就放行了），所以不会被
           持续滚动自己的注入刷屏。排查"滚一下就停"时它与 act.c 的
           "持续滚动：终止（…）"那行配对看：这一行说明"滚轮送来了什么"（轴 / 增量 /
           当时的修饰状态），那一行说明"钩子把它判成了什么"。delta=0 的占位帧
           与横向轴都要记 —— 它们正是最容易被漏掉的两类。 */
        if (g_recording.load())
            CapLog("  滚轮 %s delta=%d paused=%d caps=%d enh=%d shift=%d",
                   vert ? "纵" : "横", delta, (int)g_state.paused, (int)g_cMod,
                   (int)ModeDown(MODE_ENHANCE), (int)g_shiftMod);

        /* ★ **唯一的"控制杆"**：Caps + 增强修饰键（默认 Alt/D）按着时的滚轮。
           滚轮此时不是滚动器而是控制杆：同向滚 = 逐档调速（不再多滚一格）、
           反向滚 = 终止并把速度清零（都在 act.c 判定）。所以这一段**一律不打断**，
           并把这一格吞掉（返回 1，滚轮完全由我们接管，不会没反馈：通道一激活就
           立刻补发第一格，见 act.c）。

           判据必须盖住**整个手势**，不能只看纵向轴：横向轴（触控板双指滑动会同时
           报两个轴、倾斜滚轮也会）与 delta=0 的占位帧都是"这只手正在做的事"。
           ★ 它们过去会一路落到最后的"新输入 -> 打断"上，把刚启动的滚动当场掐掉 ——
           与"按住 Caps+Alt 滚一下就停"的症状完全吻合，是本轮改动的另一个怀疑对象
           （与"自动重复"并列，两者都靠 act.c 的终止日志才能坐实是哪一个）。 */
        if (chord && !shiftToH && ModeDown(MODE_ENHANCE)) {
            if (vert && delta != 0)
                PostMessageW(g_mainWnd, WM_WHEEL_ENH, (WPARAM)delta, 0);
            return 1;
        }

        if (shiftToH) {
            /* Caps + Shift + 滚轮 = **横向滚动**（Shift 决定轴向）。
               这一格必须**吞掉**：物理滚轮送出的是纵向事件，放行就会变成
               "横一下、纵一下"的混滚。吞掉之后横向完全由我们注入，倍数 1 也就
               恰好等于一次标准横向滚动。
               横向滚动是"另一个动作"，不是持续滚动的控制杆 -> 打断。 */
            if (delta != 0) {
                PostMessageW(g_mainWnd, WM_WHEEL_HACCEL, (WPARAM)delta, (LPARAM)hmul);
                ActAutoScrollInterrupt("横向滚轮", delta);
            }
            return 1;
        }

        /* 普通（只按住 C）：快速滚动 N 倍 —— 见 main.c 的 WM_WHEEL_ACCEL。
           物理格不吞（它是"加强"，不是"替换"）。 */
        if (vert && chord && delta != 0) {
            int mul = ConfigGetWheelScrollMul();
            if (mul > 1)
                PostMessageW(g_mainWnd, WM_WHEEL_ACCEL, (WPARAM)delta, (LPARAM)mul);
        }

        /* 走到这里的滚轮都不是控制杆（横向 / 放大 / 横滚轮 / 没按 Caps 的普通滚轮）
           -> 按"用户的新输入"打断持续滚动（用户明确要的："只有在按住 Caps+Alt 时
           同向滚动才是调速，没按住时直接打断"）。
           delta=0 的滚轮不带任何信息（部分驱动与触控板会发占位帧），不算输入 ——
           否则它就是个"什么都滚不动、却把滚动停掉"的隐形杀手。 */
        if (delta != 0) ActAutoScrollInterrupt("滚轮", delta);
    } else if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
               wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN) {
        /* 鼠标键按下也算"任意键"（用户明确要的"包含鼠标"）。
           抬起不算 —— 那是按下动作的收尾，不是新动作。 */
        ActAutoScrollInterrupt("鼠标键", (int)wParam);
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