/* ============================================================
   词跳单元测试：直接对 nav.c 的 ComputeSteps（词边界算法）断言，
   验证中文按符号/标点跳词、英文按单词跳词在左右两个方向都正确。
   通过把 nav.c 里 ComputeSteps 改为非 static 来直接调用。
   ============================================================ */
#include "cap.h"
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>

/* nav.c 内部已改为非 static 供测试调用 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir);
/* 单测未链接 main.c，补若干符号 */
volatile BOOL g_recording = FALSE;
HHOOK g_hook = NULL;   /* NavBaseInfo 会读 g_hook，需提供定义 */
void CapLog(const char* fmt, ...) { (void)fmt; }

static int fails = 0;
#define EXPECT(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s\n", name); fails++; } } while (0)

static void R(const wchar_t* s, int caret, int steps, const char* name) {
    EXPECT(ComputeSteps(s, (int)wcslen(s), caret, DIR_RIGHT) == steps, name);
}
static void L(const wchar_t* s, int caret, int steps, const char* name) {
    EXPECT(ComputeSteps(s, (int)wcslen(s), caret, DIR_LEFT) == steps, name);
}

int main(void) {
    /* 中文按符号跳词：你 好 ， 世 界 ！ 测 试（len=8） */
    const wchar_t* zh = L"你好，世界！测试";

    R(zh, 0, 3, "ZH: 头向右 0->3(跨 你好，)");
    L(zh, 6, 3, "ZH: 测 向左 6->3(跨 世界！)");
    R(zh, 3, 3, "ZH: 世界 向右 3->6(跨 世界！)");
    R(zh, 6, 2, "ZH: 测试 向右 6->8(到末尾)");
    R(zh, 8, 0, "ZH: 末尾向右 = 0（兜底）");
    L(zh, 0, 0, "ZH: 头向左 = 0（兜底）");
    R(zh, 2, 1, "ZH: 逗号处向右 2->3");
    L(zh, 5, 2, "ZH: 叹号处向左 5->3");

    /* 英文按空白词跳：hello world foo（len=15）*/
    const wchar_t* en = L"hello world foo";
    R(en, 5, 1,  "EN: 空格右 5->6(world)");
    R(en, 6, 6,  "EN: world 右 6->12(foo起)");
    L(en, 15, 3, "EN: 末尾左 15->12(foo起)");
    L(en, 6, 6, "EN: world起 左 6->0(跨 world+空格)");

    /* 代理对（如扩展G区"𰻝"=U+30E0D）必须整体算一个字符，不拆散。
       X=字母 / 𰻝=汉字 / Y=字母 → 三者分属不同类别，各成一词 */
    const wchar_t* sp = L"X𰻝Y";         /* ALPHA | CJK | ALPHA，len=4 */
    R(sp, 0, 1, "SP: X右 0->1(字母成一词)");
    R(sp, 1, 1, "SP: 𰻝右 1->3(代理对整体 1 词)");
    L(sp, 4, 1, "SP: 末尾左 4->3(跨 Y)");
    L(sp, 3, 1, "SP: 𰻝左 3->1(跨整个 𰻝)");
    L(sp, 1, 1, "SP: X左 1->0");
    L(sp, 2, 1, "SP: caret2(代理对中) 归一后 = 1");

    /* 生僻字夹在汉字中间 → 与汉字同属 CJK，连成一词（验证配对不拆） */
    const wchar_t* cj = L"中𰻝玉";       /* 中|𰻝|玉 三 CJK,jel: 中,hi,lo,玉 len=4 */
    R(cj, 0, 3, "CJ: 头右 0->尾(三 CJK 一词)");
    L(cj, 4, 3, "CJ: 末尾左 4->0(跨玉+𰻝+中)");

    /* 中英数字混排：各同类型串独立成词，类型切换处停下 */
    const wchar_t* md = L"你好，世界1234abcd中国";   /* 你0好1，2世3界4 1..4,5-8 a..d,9-12 中13国14, len=15 */
    R(md, 0, 3, "MD: 头右 0->3(跨 你好，到世界首)");
    R(md, 3, 2, "MD: 世界右 3->5(到1234首)");
    R(md, 5, 4, "MD: 1234右 5->9(到abcd首)");
    R(md, 9, 4, "MD: abcd右 9->13(到中国首)");
    R(md, 13, 2, "MD: 中国右 13->尾");
    L(md, 15, 2, "MD: 末尾左 15->13(中国词首)");
    L(md, 13, 4, "MD: 中国首左 13->9(abcd首)");
    L(md, 9, 4, "MD: abcd首左 9->5(1234首)");
    L(md, 5, 2, "MD: 1234首左 5->3(世界首)");

    /* 全角字母/数字也各自成词 */
    const wchar_t* fc = L"你好Ａ１ｂ２中国";         /* 你0好1 Ａ2 １3 ｂ4 ２5 中6国7,曾经=8 */
    R(fc, 0, 2, "FC: 头右 0->2(跨 你好到Ａ)");
    R(fc, 2, 1, "FC: Ａ右 2->3(全角字母成词)");
    R(fc, 3, 1, "FC: １右 3->4(全角数字成词)");
    R(fc, 4, 1, "FC: ｂ右 4->5");
    R(fc, 5, 1, "FC: ２右 5->6");
    L(fc, 8, 2, "FC: 末尾左 8->6(跨 中国)");
    L(fc, 6, 1, "FC: 中国首左 6->5(跨 ２)");

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}