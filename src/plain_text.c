/* ================= 纯文本粘贴的纯逻辑 =================
   见 plain_text.h 的方向说明。本文件刻意不含任何 cap.h / 剪贴板 / 配置依赖：
   它要能被单测（test/test_plainpaste.c）直接 #include 进来验证。 */

#include "plain_text.h"

BOOL PlainIsBlank(const WCHAR* s, int len) {
    if (!s || len <= 0) return TRUE;
    for (int i = 0; i < len; i++) {
        WCHAR c = s[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
            c == L'\v' || c == L'\f')
            continue;
        /* 全角空格（中文输入法下的空格）/ 不换行空格：粘出去等于什么都没粘 */
        if (c == L'\x3000' || c == L'\x00A0') continue;
        return FALSE;
    }
    return TRUE;
}

int PlainJoinPaths(const WCHAR* const* paths, int n, WCHAR* out, int cap) {
    if (!out || cap <= 0) return -1;
    out[0] = 0;
    if (!paths || n <= 0) return 0;

    int o = 0;
    for (int i = 0; i < n; i++) {
        const WCHAR* p = paths[i];
        if (!p) continue;
        /* 空路径不占一行（DragQueryFileW 不会给出空串，但别让一个空串
           变成两个连续换行） */
        if (!p[0]) continue;
        if (o > 0) {
            /* 行间分隔用 **LF**：本文件与 copy_text.c 同一套内部约定 —— 内部
               一律 LF，只在写剪贴板那一刻统一转成 CRLF（见 plainpaste.c 的
               PlainWriteText）。这里若先写 CRLF，写剪贴板时会把 \n 再展开一次，
               变成 \r\r\n，粘出去就是空行。 */
            if (o + 1 >= cap) { out[0] = 0; return -1; }
            out[o++] = L'\n';
        }
        for (int k = 0; p[k]; k++) {
            if (o + 1 >= cap) { out[0] = 0; return -1; }
            out[o++] = p[k];
        }
    }
    out[o] = 0;
    return o;
}

BOOL PlainIsConsoleClass(const WCHAR* cls) {
    if (!cls || !cls[0]) return FALSE;
    static const WCHAR* kConsoleClasses[] = {
        L"ConsoleWindowClass",              /* conhost：cmd / PowerShell / WSL 默认 */
        L"CASCADIA_HOSTING_WINDOW_CLASS",   /* Windows Terminal */
        L"mintty",                          /* Git Bash / MSYS2 */
        L"VirtualConsoleClass",             /* 部分第三方终端模拟器 */
    };
    for (int i = 0; i < (int)(sizeof(kConsoleClasses) / sizeof(kConsoleClasses[0])); i++)
        if (!_wcsicmp(cls, kConsoleClasses[i])) return TRUE;
    /* PuTTY 的类名带窗口序号（"PuTTY"、"PuTTY.EXE" 之类），按前缀认 */
    if (!_wcsnicmp(cls, L"PuTTY", 5)) return TRUE;
    return FALSE;
}
