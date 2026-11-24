/* test_plainpaste.c — 纯文本粘贴的纯逻辑（src/plain_text.c）单测。
   这一层刻意不含剪贴板 / 键盘注入 / 配置，所以能像 test_copytext 那样直接
   include 生产实现来验（见 plain_text.h 的方向说明）。要钉的是三件容易改坏的事：
     ① 空判定：空白文本不算内容（含中文输入法的全角空格 U+3000）；
     ② 路径拼接：每行一个、行间 CRLF、**末行不带换行**（粘进终端时末尾一个
        回车就变成"顺手执行了"）、不加引号、空路径不占行；
     ③ 终端类判定：控制台 / Windows Terminal / mintty / PuTTY 认得出，普通
        窗口类不能被误认（误认会让普通程序收到 Shift+Insert，多半没反应）。
   真正的剪贴板读写与注入序列不在这一层验 —— 那些要真剪贴板 + 真前台窗口，
   由 test_chord.c 的接线断言 + 手测覆盖。 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "plain_text.h"
#include "plain_text.c"     /* 纯函数：直接包含生产实现，不经链接 */

static int fails = 0;

#define EXPECT(cond, name) do {                                     \
    if (cond) printf("  [OK]   %s\n", name);                        \
    else { printf("  [FAIL] %s\n", name); fails++; }                \
} while (0)

static BOOL Blank(const WCHAR* s) { return PlainIsBlank(s, (int)wcslen(s)); }

int main(void) {
    WCHAR buf[256];

    printf("plain_text: PlainIsBlank\n");
    EXPECT(Blank(L""), "空串算空白");
    EXPECT(Blank(L" \t\r\n\v\f"), "ASCII 空白全算空白");
    EXPECT(Blank(L"\x3000"), "全角空格（中文输入法的空格）算空白");
    EXPECT(Blank(L"\x00A0"), "不换行空格算空白");
    EXPECT(!Blank(L"a"), "单字符算内容");
    EXPECT(!Blank(L" a "), "空白包着字符 -> 有内容");
    EXPECT(!Blank(L"0"), "'0' 是有内容的（别把它当空串）");
    EXPECT(!Blank(L"\n\nx\n\n"), "多行里只要有一行有字就算有内容");
    EXPECT(PlainIsBlank(NULL, 0), "NULL/0 长度算空白（调用方不用先判空）");

    printf("plain_text: PlainJoinPaths\n");
    {
        static const WCHAR* one[] = { L"C:\\a.txt" };
        int n = PlainJoinPaths(one, 1, buf, 256);
        EXPECT(n == 8 && wcscmp(buf, L"C:\\a.txt") == 0, "单个路径原样写出");
    }
    {
        static const WCHAR* many[] = { L"C:\\a.txt", L"D:\\b\\c.doc", L"\\\\.\\E:" };
        int n = PlainJoinPaths(many, 3, buf, 256);
        EXPECT(n == (int)wcslen(buf), "返回值 = 实际写入长度");
        EXPECT(wcscmp(buf, L"C:\\a.txt\nD:\\b\\c.doc\n\\\\.\\E:") == 0,
               "多路径：每行一个、行间 LF（写剪贴板时才转 CRLF）、不加引号");
        EXPECT(buf[n - 1] != L'\n', "末行不带换行（粘进终端不会顺手执行）");
    }
    {
        static const WCHAR* withEmpty[] = { L"C:\\a", L"", L"D:\\b" };
        int n = PlainJoinPaths(withEmpty, 3, buf, 256);
        EXPECT(n == 9 && wcscmp(buf, L"C:\\a\nD:\\b") == 0,
               "空路径不占一行（不会出现连续两个换行）");
    }
    {
        static const WCHAR* many[] = { L"C:\\a.txt", L"D:\\b.txt" };
        int n = PlainJoinPaths(many, 2, buf, 12);   /* 比需要的小 */
        EXPECT(n == -1 && buf[0] == 0, "容量不够 -> -1 且输出置空（不写半截）");
    }
    EXPECT(PlainJoinPaths(NULL, 3, buf, 256) == 0 && buf[0] == 0,
           "空表 -> 0 长度");
    EXPECT(PlainJoinPaths(NULL, 0, buf, 0) == -1, "cap<=0 -> -1");

    printf("plain_text: PlainIsConsoleClass\n");
    EXPECT(PlainIsConsoleClass(L"ConsoleWindowClass"), "conhost（cmd / PowerShell / WSL）");
    EXPECT(PlainIsConsoleClass(L"consolewindowclass"), "类名比较不区分大小写");
    EXPECT(PlainIsConsoleClass(L"CASCADIA_HOSTING_WINDOW_CLASS"), "Windows Terminal");
    EXPECT(PlainIsConsoleClass(L"mintty"), "Git Bash / MSYS2");
    EXPECT(PlainIsConsoleClass(L"PuTTY"), "PuTTY");
    EXPECT(PlainIsConsoleClass(L"PuTTY.EXE"), "PuTTY 的类名带序号，按前缀认");
    EXPECT(PlainIsConsoleClass(L"VirtualConsoleClass"), "第三方终端模拟器");
    EXPECT(!PlainIsConsoleClass(L"Notepad"), "记事本不是终端（要 Ctrl+V）");
    EXPECT(!PlainIsConsoleClass(L"CASCADIA_HOSTING_WINDOW_CLAS"), "前缀不足不算");
    EXPECT(!PlainIsConsoleClass(L"ConsoleWindowClassX"), "多一个字符也不算");
    EXPECT(!PlainIsConsoleClass(L""), "空类名不算");
    EXPECT(!PlainIsConsoleClass(NULL), "NULL 不算");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
