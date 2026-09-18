/* ============================================================
   test_color.c — colorcode 的算法单测（照 Ditto 判据，钉住边界）

   为什么要有：色值的分支很多（命名色 / #hex / 0x / rgb() / hsl() / 括号 /
   逗号 / 空格 / 6 位纯 hex），而且"差一点"就会静默变成普通文本 —— 不会崩、
   不会报错，只是色块不出现。这类只能靠单测钉住。
   ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "../src/colorcode.h"

static int fails = 0;

/* 期望解析成功，并核对 RGB 与 alpha */
static void Ok(const WCHAR* in, int r, int g, int b, int a, const char* name) {
    COLORREF rgb = 0; int alpha = -1;
    BOOL got = CcParseColor(in, &rgb, &alpha);
    int rr = GetRValue(rgb), gg = GetGValue(rgb), bb = GetBValue(rgb);
    if (got && rr == r && gg == g && bb == b && alpha == a) {
        printf("  [OK]   %s\n", name);
    } else {
        printf("  [FAIL] %s  期望 (%d,%d,%d,a=%d) 实际 %s(%d,%d,%d,a=%d)\n",
               name, r, g, b, a, got ? "得到" : "未识别", rr, gg, bb, alpha);
        fails++;
    }
}

/* 期望解析失败（普通文本） */
static void No(const WCHAR* in, const char* name) {
    COLORREF rgb = 0; int alpha = -1;
    BOOL got = CcParseColor(in, &rgb, &alpha);
    if (!got) {
        printf("  [OK]   %s\n", name);
    } else {
        printf("  [FAIL] %s  期望不识别，实际得到 (%d,%d,%d,a=%d)\n",
               name, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb), alpha);
        fails++;
    }
}

int main(void) {
    /* ---- hex（有前缀） ---- */
    Ok(L"#F45F01",   244, 95,  1,   255, "hex: #F45F01");
    Ok(L"#f45f01",   244, 95,  1,   255, "hex: 小写同样识别");
    Ok(L"0xF45F01",  244, 95,  1,   255, "hex: 0x 前缀");
    Ok(L"#FFF",      255, 255, 255, 255, "hex: #FFF 三位简写展开");
    Ok(L"#F45",      255, 68,  85,  255, "hex: #F45 三位简写展开");
    Ok(L"#F45F0180", 244, 95,  1,   128, "hex: 八位带 alpha");
    Ok(L"#FF0000FF", 255, 0,   0,   255, "hex: 八位 alpha=FF");

    /* ---- hex（无前缀，用户最常复制的形态） ---- */
    Ok(L"F45F01",    244, 95,  1,   255, "hex: 无前缀六位");
    Ok(L"ff0000",    255, 0,   0,   255, "hex: 无前缀小写");

    /* ---- 清理规则（照 Ditto：»、空白、右侧分号） ---- */
    Ok(L"»#F45F01",  244, 95,  1,   255, "清理: 前导 »");
    Ok(L"  #F45F01  ", 244, 95, 1,  255, "清理: 首尾空白");
    Ok(L"#F45F01;",  244, 95,  1,   255, "清理: 右侧分号");

    /* ---- W3C 命名色 ---- */
    Ok(L"red",       255, 0,   0,   255, "名: red");
    Ok(L"Red",       255, 0,   0,   255, "名: 大小写不敏感");
    Ok(L"cornflowerblue", 100, 149, 237, 255, "名: cornflowerblue");
    Ok(L"yellowgreen", 154, 205, 50, 255, "名: yellowgreen（表尾二分）");

    /* ---- rgb() / rgba() ---- */
    Ok(L"rgb(255, 128, 0)",      255, 128, 0, 255, "rgb: 整数");
    Ok(L"rgb(255 128 0)",        255, 128, 0, 255, "rgb: 空格分隔");
    Ok(L"rgba(255, 128, 0, 0.5)", 255, 128, 0, 128, "rgba: alpha 0.5");
    /* ★ 50% 照 Ditto 是 **127**：它算的是 50 * 2.55，double 下等于
       127.49999999999999，round 之后就是 127（不是理想值 127.5→128）。
       我们照抄这个行为 —— 与 Ditto 显示同一个色，比"数学上更对"更重要。 */
    Ok(L"rgb(100%, 50%, 0%)",    255, 127, 0, 255, "rgb: 百分比（50% -> 127，与 Ditto 一致）");

    /* ---- hsl() / hsla() ---- */
    Ok(L"hsl(120, 100%, 50%)",    0,   255, 0,   255, "hsl: 纯绿");
    Ok(L"hsl(0, 100%, 50%)",      255, 0,   0,   255, "hsl: 纯红");
    Ok(L"hsla(0, 100%, 50%, 0.5)", 255, 0,  0,   128, "hsla: 带 alpha");
    Ok(L"hsl(120deg, 100%, 50%)", 0,   255, 0,   255, "hsl: 色相带 deg");

    /* ---- 三个整数的其它写法 ---- */
    Ok(L"(255, 128, 0)", 255, 128, 0, 255, "裸 RGB: 括号");
    Ok(L"255, 0, 0",     255, 0,   0, 255, "裸 RGB: 逗号");
    Ok(L"255 128 0",     255, 128, 0, 255, "裸 RGB: 空格");

    /* ---- 不该识别 ---- */
    No(L"",            "否: 空串");
    No(L"hello",       "否: 普通词");
    No(L"#F45F0",      "否: 五位 hex");
    No(L"#GGGGGG",     "否: 非 hex 字符");
    No(L"256, 0, 0",   "否: 分量越界");
    No(L"255, 0",      "否: 只有两个分量");
    No(L"#F45F01 主色", "否: 色值后面还有文字");
    No(L"rgb(255, 128)", "否: rgb 缺分量");
    No(L"hsl(120, 100%)", "否: hsl 缺分量");
    No(L"255.5, 0, 0", "否: 非整数分量");

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}
