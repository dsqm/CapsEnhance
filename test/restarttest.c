/* restarttest.c - 验证"重启程序"机制（--wait-exit 握手）。
 *
 * 流程：
 *   1. 启动实例 A，等它的 CapsEnhanceMain 窗口出现，记 hwnd_A
 *   2. 启动实例 B（--wait-exit <A_pid>）
 *   3. A 活着时：FindWindow 应仍返回 hwnd_A（B 在等待，未初始化）
 *   4. 结束 A：B 应在短延时内接管 —— FindWindow 返回非 0 且 != hwnd_A
 *   5. 向 B 发 WM_CLOSE 干净退出
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static int fails = 0;
#define EXPECT(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s\n", name); fails++; } } while (0)

static HWND WaitMainWindow(int timeoutMs) {
    ULONGLONG end = GetTickCount64() + (ULONGLONG)timeoutMs;
    while (GetTickCount64() < end) {
        HWND h = FindWindowW(L"CapsEnhanceMain", NULL);
        if (h) return h;
        Sleep(100);
    }
    return NULL;
}

/* 等到"窗口换了一个"：重启场景下旧窗口会先消失再出现新的，
   只看"有没有窗口"会在旧窗口还没销毁时就返回旧句柄 —— 那是假通过。 */
static HWND WaitNewWindow(HWND oldHwnd, int timeoutMs) {
    ULONGLONG end = GetTickCount64() + (ULONGLONG)timeoutMs;
    while (GetTickCount64() < end) {
        HWND h = FindWindowW(L"CapsEnhanceMain", NULL);
        if (h && h != oldHwnd) return h;
        Sleep(100);
    }
    return NULL;
}

int main(void) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    /* 去掉 restarttest.exe，换成 CapsEnhance.exe（同目录） */
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) *(slash + 1) = 0;
    wcscat_s(exe, MAX_PATH, L"CapsEnhance.exe");

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, exe, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        wprintf(L"cannot start CapsEnhance.exe (path='%s') GetLastError=%lu\n",
                exe, GetLastError());
        return 2;
    }
    DWORD pidA = pi.dwProcessId;
    HANDLE hA = pi.hProcess;
    CloseHandle(pi.hThread);

    HWND hwndA = WaitMainWindow(3000);
    EXPECT(hwndA != NULL, "A 启动且主窗口出现");

    /* B：等 A 退出后再初始化 */
    WCHAR cmd[MAX_PATH + 64];
    _snwprintf_s(cmd, MAX_PATH + 64, _TRUNCATE,
                 L"\"%s\" --wait-exit %lu", exe, (unsigned long)pidA);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    BOOL okB = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    HANDLE hB = pi.hProcess;
    CloseHandle(pi.hThread);
    EXPECT(okB, "B 带 --wait-exit 启动成功");
    if (okB) wprintf(L"B pid=%lu (wait-exit %lu)\n",
                     (unsigned long)pi.dwProcessId, (unsigned long)pidA);

    Sleep(1500);
    HWND now = FindWindowW(L"CapsEnhanceMain", NULL);
    EXPECT(now == hwndA, "A 活着时 B 未初始化（窗口仍是 A 的）");

    /* 结束 A */
    TerminateProcess(hA, 0);
    WaitForSingleObject(hA, 3000);
    Sleep(500);

    HWND hwndB = WaitMainWindow(3000);
    EXPECT(hwndB != NULL && hwndB != hwndA, "A 退出后 B 接管（新窗口出现）");

    /* 单实例：B 还活着时再启动一个不带参数的 C，C 应自行退出 */
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    BOOL okC = CreateProcessW(exe, exe, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    HANDLE hC = pi.hProcess;
    CloseHandle(pi.hThread);
    EXPECT(okC, "C 启动成功（不带参数）");
    if (okC) {
        DWORD rc = WaitForSingleObject(hC, 3000);
        EXPECT(rc == WAIT_OBJECT_0, "已有实例在跑时 C 自动退出（单实例）");
    }
    EXPECT(FindWindowW(L"CapsEnhanceMain", NULL) == hwndB,
           "C 退出后窗口仍是 B 的（两个实例不并存）");
    if (hC) CloseHandle(hC);

    /* ---- --restart：命令行请求"已经在跑的那个实例"重启 ----
       设置程序保存配置后走的就是这条路：它不认识本体的内部消息，只按约定
       启动一次 `CapsEnhance.exe --restart`。要点有两个：
         · 带 --restart 启动的那个实例自己**不该**变成第二个常驻实例
           （它只负责转告，然后退出 —— 否则就破了单实例）；
         · 在跑的实例必须真的重启（窗口换了），否则"改了配置没生效"。 */
    {
        WCHAR rcmd[MAX_PATH + 64];
        _snwprintf_s(rcmd, MAX_PATH + 64, _TRUNCATE, L"\"%s\" --restart", exe);
        STARTUPINFOW si2; PROCESS_INFORMATION pi2;
        memset(&si2, 0, sizeof(si2)); si2.cb = sizeof(si2);
        memset(&pi2, 0, sizeof(pi2));
        BOOL okR = CreateProcessW(exe, rcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi2);
        EXPECT(okR, "R: --restart 实例启动成功");
        if (okR) {
            CloseHandle(pi2.hThread);
            DWORD rcR = WaitForSingleObject(pi2.hProcess, 3000);
            EXPECT(rcR == WAIT_OBJECT_0,
                   "R: --restart 实例转告后立即退出（不变成第二个常驻实例）");
            /* 超时放宽到 10s：默认配置 RunAsAdmin=1 时，新实例会先走一次
               提权重启（UAC），窗口出现得比"直接起来"晚一些。 */
            HWND hwndD = WaitNewWindow(hwndB, 10000);
            EXPECT(hwndD != NULL, "R: --restart 后由新实例接管（窗口换了）");
            EXPECT(hB && WaitForSingleObject(hB, 3000) == WAIT_OBJECT_0,
                   "R: 收到请求的实例确实退出了（不是又开一个并存）");
            /* 收尾：让重启出来的实例干净退出 */
            if (hwndD) PostMessageW(hwndD, WM_CLOSE, 0, 0);
            CloseHandle(pi2.hProcess);
        }
    }

    /* 收尾：让 B 干净退出（托盘/钩子正常清理） */
    if (hwndB) PostMessageW(hwndB, WM_CLOSE, 0, 0);
    if (hB) { WaitForSingleObject(hB, 3000); CloseHandle(hB); }
    CloseHandle(hA);

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}
