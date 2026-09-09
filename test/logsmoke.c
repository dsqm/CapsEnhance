/* logsmoke.c - 冒烟验证 CapLog 的日志落盘（log_path 宽字符路径 + _wfopen）。
 * 启动 CapsEnhance.exe -> 投递 WM_PROBE（等价 C+` 开始记录）-> 确认日志
 * 出现 "REC START" -> 结束进程。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(void) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) *(slash + 1) = 0;
    wcscat_s(exe, MAX_PATH, L"CapsEnhance.exe");

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, exe, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("cannot start\n"); return 2;
    }
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    HWND h = NULL;
    for (int i = 0; i < 30 && !h; i++) {
        h = FindWindowW(L"CapsEnhanceMain", NULL);
        if (!h) Sleep(100);
    }
    if (!h) { printf("no window\n"); TerminateProcess(pi.hProcess, 2); CloseHandle(pi.hProcess); return 2; }
    Sleep(300);

    /* WM_PROBE = WM_APP+6：等价 C+` 开始记录（CapLog("===== REC START =====")） */
    PostMessageW(h, WM_APP + 6, 0, 0);
    Sleep(800);

    int ok = 0;
    FILE* f = NULL;
    if (fopen_s(&f, "bin/CapsEnhance.log", "r") == 0 && f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "REC START")) { ok = 1; break; }
        }
        fclose(f);
    }
    printf(ok ? "[OK]   log written via wide path (_wfopen)\n"
              : "[FAIL] log not written\n");

    /* NavProbe 会弹 MessageBox 阻塞主线程，直接结束进程即可 */
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    DeleteFileA("bin/CapsEnhance.log");   /* 清掉冒烟日志 */
    return ok ? 0 : 1;
}
