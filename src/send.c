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

/* 单条键盘事件（按下 / 抬起） */
static void SendEvent(int vk, BOOL up) {
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

/* 逐方向键移动：发送 n 个左/右箭头（可带 Shift 位移）。
   每个箭头单独发送并微小间隔，避免大批量 SendInput 在部分程序中只认第一个键；
   Shift 在整个跳词期间持续按住以维持选区。 */
#define MAX_ARROW_STEPS 256
void SendArrows(int dir, int n, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(KEY_GAP); }
    for (int i = 0; i < n && i < MAX_ARROW_STEPS; i++) {
        SendEvent(vk, FALSE);
        Sleep(KEY_GAP);
        SendEvent(vk, TRUE);
        Sleep(ARROW_GAP);
    }
    if (shift) SendEvent(VK_LSHIFT, TRUE);
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

/* 重启 / 退出时清键盘状态：对我们可能注入过的修饰键逐个补"抬起"。
   卡键的典型成因是和弦进行到一半程序退出或目标吃掉键，修饰键停在下位。
   只补异步状态里"当前没被物理按着"的键，不破坏用户真实按住的修饰键。 */
void SendClearStuckModifiers(void) {
    static const int vks[] = {
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
        VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN,
        VK_CAPITAL
    };
    for (int i = 0; i < (int)(sizeof(vks) / sizeof(vks[0])); i++) {
        if (!(GetAsyncKeyState(vks[i]) & 0x8000))
            SendEvent(vks[i], TRUE);
    }
}
