#include "cap.h"

/* ================= 模拟按键发送 =================
   这一层只关心"把键正确地送出去"，不含任何业务逻辑。三条硬规则都是踩过坑的：

   1) 扩展键必须带 KEYEVENTF_EXTENDEDKEY。方向键/Home/End/Del 等的扫描码
      与小键盘共用，缺了扩展标志，目标程序拿到的是"小键盘 4/6"而不是
      "左/右箭头"。实测（Chromium）：不加标志时 keydown 的 code=Numpad4 /
      Numpad6，依赖 code 派发的程序（VSCode）就会认错键；加上后 code=ArrowLeft。
      反过来 VK_RETURN 绝不能加 —— 主键盘回车不扩展，加了会变成小键盘回车。

   2) 逐条 SendInput + 微小间隔，而不是一次塞一整批。批量发送在部分程序里
      只认第一个键（这也是 SendArrows 一直逐条发的原因）。

   3) 组合键里修饰键必须真的按住：Shift/Ctrl 全程在目标键之前按下、之后抬起。
      之前兜底词跳把 Ctrl↓ Ctrl↑ 连续发出，目标收到的方向键 ctrl=0，
      于是退化成"单格方向键"——这正是 caps+N/M 在多个程序里"只挪一格"的原因。
*/

/* 需要 KEYEVENTF_EXTENDEDKEY 的键 */
static BOOL IsExtendedVk(int vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE:
    case VK_SNAPSHOT:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_DIVIDE:                 /* 小键盘 / */
        return TRUE;
    default:
        return FALSE;
    }
}

/* 我们注入过"按下"但还没配"抬起"的修饰键：进程在组合键中途退出/重启时
   会留下真正的卡键，退出前无条件补发抬起（与用户是否物理按住无关，
   因为这些键本来就只该由我们按下）。 */
#define MOD_COUNT 8
static const int kMods[MOD_COUNT] = {
    VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
    VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN
};
static BOOL g_injectedDown[MOD_COUNT] = { 0 };

static int ModIndex(int vk) {
    for (int i = 0; i < MOD_COUNT; i++) if (kMods[i] == vk) return i;
    return -1;
}

/* 单条键盘事件（按下 / 抬起） */
static void SendEvent(int vk, BOOL up) {
    int mi = ModIndex(vk);
    if (mi >= 0) g_injectedDown[mi] = !up;
    INPUT i;
    memset(&i, 0, sizeof(i));
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = (WORD)vk;
    i.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (IsExtendedVk(vk)) i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up)                i.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &i, sizeof(INPUT));
}

/* 事件之间的间隔：让目标程序的消息队列逐个消化 */
#define KEY_GAP        5      /* ms：组合键内部 */
#define ARROW_GAP      8      /* ms：连续箭头之间 */

/* 单击一个键；shift 全程按住（Shift↓ 键↓ 键↑ Shift↑） */
void SendKeyOnce(int vk, BOOL shift) {
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(KEY_GAP); }
    SendEvent(vk, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, TRUE);
    if (shift) { Sleep(KEY_GAP); SendEvent(VK_LSHIFT, TRUE); }
}

/* 逐格输出的打断机制：
   SendArrows 的循环跑在主线程，期间主线程被占用、无法处理任何消息，
   所以"松开 C"和"改道按键"这些打断意图只能由钩子线程置 volatile 标志，
   循环里每发完一对键检查一次（最坏 ~13ms 内响应）。
   触发条件（hook.c 调用）：
     1) C（CapsLock）松开 —— 映射模式结束，立即停；
     2) 按下与当前输出方向不同的触发键 —— 改道（如正在 ← 时按 L）。
   打断后循环立即退出，循环外的收尾（抬起 Shift）照常执行；
   改道按键的 TRIG_DOWN 已在主线程队列里，循环结束后自然开火衔接。 */
static volatile BOOL g_arrBusy = FALSE;    /* 正在逐格输出 */
static volatile int  g_arrDir  = -1;       /* 当前输出方向 (DIR_*) */
static volatile BOOL g_arrBreak = FALSE;   /* 打断请求 */

void SendArrowsRequestInterrupt(void) { g_arrBreak = TRUE; }

/* act 是否与正在逐格输出的方向冲突（不同触发键即改道） */
BOOL SendArrowsConflicts(int act) {
    if (!g_arrBusy) return FALSE;
    int curAct = (g_arrDir == DIR_LEFT) ? ACT_WORD_L : ACT_WORD_R;
    return act != curAct;
}

/* 逐方向键移动：发送 n 个左/右箭头（可带 Shift 位移）。
   每个箭头单独发送并微小间隔，避免大批量 SendInput 在部分程序中只认第一个键；
   Shift 在整个跳词期间持续按住以维持选区。 */
#define MAX_ARROW_STEPS 256
void SendArrows(int dir, int n, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    g_arrDir = dir; g_arrBusy = TRUE; g_arrBreak = FALSE;
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(KEY_GAP); }
    for (int i = 0; i < n && i < MAX_ARROW_STEPS; i++) {
        if (g_arrBreak) break;
        SendEvent(vk, FALSE);
        Sleep(KEY_GAP);
        if (g_arrBreak) break;
        SendEvent(vk, TRUE);
        if (g_arrBreak) break;
        Sleep(ARROW_GAP);
    }
    g_arrBusy = FALSE;
    if (shift) SendEvent(VK_LSHIFT, TRUE);   /* 打断时也要抬 Shift，选区停在此处 */
}

/* 兜底：文字读取失败时，用系统原生 Ctrl(+Shift)+方向键做词跳。
   Ctrl 必须在方向键之前按下、之后抬起，否则目标收到的只是单格方向键。 */
void SendNativeWordJump(int dir, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(KEY_GAP); }
    SendEvent(VK_LCONTROL, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, TRUE);
    Sleep(KEY_GAP);
    SendEvent(VK_LCONTROL, TRUE);
    if (shift) { Sleep(KEY_GAP); SendEvent(VK_LSHIFT, TRUE); }
}

/* 释放一个修饰键。Alt / Win 的"抬起"被系统单独看到时会被解释为
   "按下并释放了 Alt/Win" —— 从而激活菜单栏或开始菜单（AHK 为此专门
   引入 #MenuMaskKey：用一个中性键插在中间，让系统认为中间按过别的键）。
   这里用未定义虚拟键 0xFF 作掩码键，行为与 AHK 的 vkFF 一致。 */
#define MASK_VK 0xFF
static void ReleaseMod(int vk) {
    if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) {
        SendEvent(MASK_VK, FALSE);
        SendEvent(MASK_VK, TRUE);
    }
    SendEvent(vk, TRUE);
}

/* 重启 / 退出时清键盘状态（AHK 同理：区分"逻辑状态"与"物理状态"）。
   GetAsyncKeyState 反映的是 OS 的逻辑状态——被注入的键同样置位，
   所以卡键恰恰表现为"OS 认为它按着"。判据是：
       逻辑按下（OS 认为按着） && 物理未按（用户没真的按） == 卡键
   物理状态只能由低级钩子自己记录（见 hook.c 的 PhysKeyDown）。
   注：此前写成"逻辑未按才补发抬起"，等于把所有真正卡住的键全跳过了，
   这就是"重启程序清不掉卡键"的原因。 */
void SendClearStuckModifiers(void) {
    for (int i = 0; i < MOD_COUNT; i++) {
        int vk = kMods[i];
        BOOL logicalDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (logicalDown && !PhysKeyDown(vk)) ReleaseMod(vk);
    }
    /* 我们注入过的修饰键：无条件补抬起（这些键本不该残留 down 状态） */
    for (int i = 0; i < MOD_COUNT; i++)
        if (g_injectedDown[i]) ReleaseMod(kMods[i]);
    /* CapsLock 是开关键，逻辑状态表示"灯亮"，不是卡键；只在明确没物理按住
       且灯亮时才切一次，避免把用户的大小写状态改乱 —— 保守起见不处理。 */
}

/* 强制重置键盘状态（命令行 --reset-keys）：不看任何状态，对所有修饰键
   各补一次抬起。用于程序已被杀死、状态表丢失的场合（等价 AHK 社区常用的
   "把所有修饰键 Send Up 一遍"）。 */
void SendForceResetKeys(void) {
    for (int i = 0; i < MOD_COUNT; i++) ReleaseMod(kMods[i]);
}
