/* test_copytext.c — 控件复制的纯文本处理（src/copy_text.c）单测。
   这一层刻意不含 COM / 窗口 / 配置，所以能像 nav_word 那样直接 include
   生产实现来验（见 copy_text.h 的方向说明）。要钉的是三件容易改坏的事：
     ① 换行统一：CRLF 与裸 CR 都要变成 LF，且**只**在这一层统一；
     ② 去尾：只去尾部，**内部换行与缩进一个都不能动**；
     ③ 预览截断：行优先、字兜底，且"没被截"时不能谎报 truncated。
   剪贴板永远写全文这件事由 textgrab.c 的调用顺序保证，不在这层验。 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "copy_text.h"
#include "copy_text.c"      /* 纯函数：直接包含生产实现，不经链接 */

static int fails = 0;

#define EXPECT(cond, name) do {                                     \
    if (cond) printf("  [OK]   %s\n", name);                        \
    else { printf("  [FAIL] %s\n", name); fails++; }                \
} while (0)

static int Clean(const WCHAR* src, BOOL strip, WCHAR* out, int cap) {
    return CopyClean(src, (int)wcslen(src), strip, out, cap);
}

int main(void) {
    WCHAR buf[256];

    printf("copy_text: CopyClean\n");
    EXPECT(Clean(L"A\r\nB\r\nC", TRUE, buf, 256) == 5 && wcscmp(buf, L"A\nB\nC") == 0,
           "CRLF 全部变成 LF");
    EXPECT(Clean(L"A\rB\rC", TRUE, buf, 256) == 5 && wcscmp(buf, L"A\nB\nC") == 0,
           "裸 CR 也变成 LF");
    EXPECT(Clean(L"x\n\n  \t\n", TRUE, buf, 256) == 1 && wcscmp(buf, L"x") == 0,
           "去尾：尾部换行 / 空格 / 制表符一并去掉");
    EXPECT(Clean(L"x\n\n  \t\n", FALSE, buf, 256) == 7,
           "去尾关闭时尾部原样保留");
    EXPECT(Clean(L"  a\n    b\n", TRUE, buf, 256) == 9 && wcscmp(buf, L"  a\n    b") == 0,
           "内部换行与缩进不受影响（只砍尾部）");
    EXPECT(Clean(L"", TRUE, buf, 256) == 0 && buf[0] == 0, "空串 -> 空串");
    EXPECT(Clean(L"   ", TRUE, buf, 256) == 0, "纯空白 -> 空串（由调用方判失败）");

    printf("copy_text: CopyPreviewLen\n");
    {
        BOOL cut = TRUE;
        int n = CopyPreviewLen(L"a\nb\nc\nd", 7, 100, 2, &cut);
        EXPECT(n == 3 && cut, "行优先：超过 2 行就在第 2 行末截断");
        EXPECT(n == 3 && wcsncmp(L"a\nb\nc\nd", L"a\nb", (size_t)n) == 0, "截断位置正好是第二个换行前");
    }
    {
        BOOL cut = FALSE;
        int n = CopyPreviewLen(L"abcdef", 6, 3, 10, &cut);
        EXPECT(n == 3 && cut, "字兜底：行没超但字数超了也要截");
    }
    {
        BOOL cut = TRUE;
        int n = CopyPreviewLen(L"abc", 3, 500, 10, &cut);
        EXPECT(n == 3 && !cut, "都没超 -> 不截，truncated 必须是 FALSE");
    }
    {
        BOOL cut = FALSE;
        int n = CopyPreviewLen(L"abcdef", 6, 0, 0, &cut);
        EXPECT(n == 1 && cut, "非法的 0 行 / 0 字按下限 1 处理，不会返回 0 长度");
    }
    {
        BOOL cut = TRUE;
        int n = CopyPreviewLen(L"", 0, 500, 10, &cut);
        EXPECT(n == 0 && !cut, "空串不截");
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
