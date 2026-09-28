/* Hyper 误触观察探针（诊断用）：
   直接数桌面上的资源管理器窗口（CabinetWClass）：注入 Hyper 四键 + E 之前/之后
   各枚举一次。若系统的 Win+E 在四键齐按时仍会触发，就会冒出新窗口。 */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static int CountExplorer(void) {
    int n = 0;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        WCHAR cls[64];
        if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"CabinetWClass") == 0 &&
            IsWindowVisible(h)) (*(int*)lp)++;
        return TRUE;
    }, (LPARAM)&n);
    return n;
}

static void KeyEv(int vk, BOOL up) {
    INPUT i; memset(&i, 0, sizeof(i));
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = (WORD)vk;
    i.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (vk == VK_RWIN || vk == VK_RCONTROL || vk == VK_RMENU) i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up) i.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &i, sizeof(INPUT));
}

int main(void) {
    const int four[4] = { VK_RWIN, VK_RCONTROL, VK_RSHIFT, VK_RMENU };
    for (int round = 0; round < 2; round++) {
        int before = CountExplorer();
        HWND fg0 = GetForegroundWindow();
        for (int k = 0; k < 4; k++) { KeyEv(four[k], FALSE); Sleep(25); }
        /* 第 1 轮按 E（Win+E 的目标键），第 2 轮按 1（Win+1 任务栏） */
        int vk = (round == 0) ? 'E' : '1';
        KeyEv(vk, FALSE); Sleep(40); KeyEv(vk, TRUE); Sleep(40);
        for (int k = 3; k >= 0; k--) { KeyEv(0xFF, FALSE); KeyEv(0xFF, TRUE); KeyEv(four[k], TRUE); Sleep(25); }
        Sleep(900);
        int after = CountExplorer();
        HWND fg1 = GetForegroundWindow();
        printf("轮%d 目标键'%c'：资源管理器窗口 %d -> %d，前台窗口%s\n",
               round + 1, vk, before, after,
               (fg0 != fg1) ? "变了" : "没变");
    }
    return 0;
}
