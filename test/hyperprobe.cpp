/* Hyper 键集事实探针（诊断用，不进源码清单）：
   验证"按住 RWin+RCtrl+RShift+RAlt 四键（Hyper）时，Win+E / Win+1 这类
   系统组合键还触不触发"。原理：本进程自己 RegisterHotKey(MOD_WIN, 'E'/'1')
   —— 资源管理器的 Win+E / 任务栏的 Win+数字就是这条路注册的 —— 然后注入
   修饰键 + 目标键，看 WM_HOTKEY 到不到。
     测试1（对照组）：只按 RWin + E       —— MOD_WIN 热键必须触发，否则探针无效
     测试2（关键）：四键齐按 + E          —— 触发 = 系统热键不看多余修饰键（Win 危险）
                                             不触发 = RegisterHotKey 精确匹配（Hyper 含 Win 可行）
     测试3（关键）：四键齐按 + 1          —— 同上，任务栏类
     测试4（对照组2）：RWin+RCtrl+RShift+RAlt + E —— 完整 Hyper 组合自己的热键（MOD_WIN|CTRL|SHIFT|ALT）
   全程只注入、不改任何系统设置；测试 2/3 若系统热键真的会触发，代价是可能
   弹一个资源管理器窗口（可关）。 */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static const int kRWin = VK_RWIN, kRCtrl = VK_RCONTROL, kRShift = VK_RSHIFT, kRAlt = VK_RMENU;

static void KeyEv(int vk, BOOL up) {
    INPUT i; memset(&i, 0, sizeof(i));
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = (WORD)vk;
    i.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (vk == kRWin || vk == kRCtrl || vk == kRAlt) i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up) i.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (!SendInput(1, &i, sizeof(INPUT))) printf("  !! SendInput 被拒 vk=0x%02X\n", vk);
}
static void MaskKey(void) {           /* AHK #MenuMaskKey 同款：中性键隔开 Win 的抬起 */
    KeyEv(0xFF, FALSE); Sleep(15); KeyEv(0xFF, TRUE); Sleep(15);
}
static void Hold(const int* vks, int n)  { for (int k = 0; k < n; k++) { KeyEv(vks[k], FALSE); Sleep(20); } }
static void Release(const int* vks, int n) { MaskKey(); for (int k = n - 1; k >= 0; k--) { KeyEv(vks[k], TRUE); Sleep(20); } }
static void TapKey(int vk) { KeyEv(vk, FALSE); Sleep(30); KeyEv(vk, TRUE); Sleep(30); }

static int g_hot[8];
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_HOTKEY && wp < 8) { g_hot[wp]++; return 0; }
    return DefWindowProcW(h, m, wp, lp);
}

int main(void) {
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CapsHyperProbe";
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"CapsHyperProbe", L"hyper-probe", WS_OVERLAPPEDWINDOW,
                           40, 40, 240, 120, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    Sleep(300);

    /* MOD_NOREPEAT 无所谓；MOD_WIN = 0x4, MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=0x4? 不：
       MOD_ALT=0x1, MOD_CONTROL=0x2, MOD_SHIFT=0x4, MOD_WIN=0x8 */
    if (!RegisterHotKey(w, 1, MOD_WIN, 'E'))                       printf("!! 注册热键1失败\n");
    if (!RegisterHotKey(w, 2, MOD_WIN, '1'))                       printf("!! 注册热键2失败\n");
    if (!RegisterHotKey(w, 3, MOD_WIN | MOD_CONTROL | MOD_SHIFT | MOD_ALT, 'E')) printf("!! 注册热键3失败\n");

    const int four[4] = { kRWin, kRCtrl, kRShift, kRAlt };
    const int winonly[1] = { kRWin };

    struct { int id; const char* what; } cases[] = {
        { 1, "测试1 对照组: RWin+E        -> 热键[Win+E]" },
        { 1, "测试2 关键:   Hyper四键+E   -> 热键[Win+E]" },
        { 2, "测试3 关键:   Hyper四键+1   -> 热键[Win+1]" },
        { 3, "测试4 对照组: Hyper四键+E   -> 热键[Win+Ctrl+Shift+Alt+E]" },
    };
    for (int c = 0; c < 4; c++) {
        memset(g_hot, 0, sizeof(g_hot));
        if (c == 1 || c == 2 || c == 3) { Hold(four, 4); Sleep(80); }
        else                            { Hold(winonly, 1); Sleep(80); }
        TapKey((c == 2) ? '1' : 'E');
        if (c == 1 || c == 2 || c == 3) Release(four, 4);
        else                            Release(winonly, 1);
        /* 让 WM_HOTKEY 飞一会儿 */
        MSG msg; DWORD until = GetTickCount() + 600;
        while (GetTickCount() < until) { while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } Sleep(20); }
        printf("%s : %s\n", cases[c].what, g_hot[cases[c].id] ? "触发了" : "没触发");
        Sleep(200);
    }

    UnregisterHotKey(w, 1); UnregisterHotKey(w, 2); UnregisterHotKey(w, 3);
    /* 兜底：所有修饰键一律补抬起，防卡键 */
    const int all[4] = { kRWin, kRCtrl, kRShift, kRAlt };
    Release(all, 4);
    DestroyWindow(w);
    return 0;
}
