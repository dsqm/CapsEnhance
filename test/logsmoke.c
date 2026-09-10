/* logsmoke.c - 冒烟验证 CapLog 的日志落盘（log_path 宽字符路径 + _wfopen）。
 * 启动 CapsEnhance.exe -> 投递 WM_PROBE（等价 C+` 开始记录）-> 确认日志
 * 出现 "REC START" -> 结束进程。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* 文件字节数（失败返回 -1） */
static long long file_size(const char* p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | (long long)d.nFileSizeLow;
}

int main(void) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) *(slash + 1) = 0;
    wcscat_s(exe, MAX_PATH, L"CapsEnhance.exe");

    /* 预置一个超上限的日志（默认 LogMaxKB=2048），验证写日志时会自动轮转 */
    FILE* big = NULL;
    if (fopen_s(&big, "bin/CapsEnhance.log", "wb") == 0 && big) {
        /* 写真实文本行（约 3MB），而不是 NUL 填充——更接近实际日志 */
        for (int i = 0; i < 42000; i++)
            fprintf(big, "%08d padding line to grow the log file, xxxxxxxxxxxxxxxxxxxxxxxx\n", i);
        fputs("LOGROTATE-TAIL-MARKER last line of the old log\n", big);
        fclose(big);
    }
    long long before = file_size("bin/CapsEnhance.log");

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

    /* 轮转：超过 LogMaxKB 后应保留最近的一半，并留下标记行 */
    long long after = file_size("bin/CapsEnhance.log");
    int rotated = 0, keptTail = 0;
    FILE* f2 = NULL;
    if (fopen_s(&f2, "bin/CapsEnhance.log", "r") == 0 && f2) {
        char line[512];
        while (fgets(line, sizeof(line), f2)) {
            if (strstr(line, "日志已达上限")) rotated = 1;
            if (strstr(line, "LOGROTATE-TAIL-MARKER")) keptTail = 1;
        }
        fclose(f2);
    }
    int sizeOK = (before > 2 * 1024 * 1024) && (after > 0) && (after <= 2048LL * 1024);
    printf(sizeOK ? "[OK]   log rotated: %lld KB -> %lld KB (<= LogMaxKB)\n"
                  : "[FAIL] log size not trimmed (before=%lld after=%lld)\n",
           before / 1024, after / 1024, before, after);
    printf(rotated ? "[OK]   rotation marker written\n"
                   : "[FAIL] rotation marker missing\n");
    printf(keptTail ? "[OK]   recent content kept\n"
                    : "[FAIL] recent content lost\n");
    if (!sizeOK || !rotated || !keptTail) ok = 0;

    /* NavProbe 会弹 MessageBox 阻塞主线程，直接结束进程即可 */
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    DeleteFileA("bin/CapsEnhance.log");   /* 清掉冒烟日志 */
    return ok ? 0 : 1;
}
