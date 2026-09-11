/* ============================================================
   词跳单元测试：直接对 nav.c 的 ComputeSteps（词边界算法）断言，
   验证中文按符号/标点跳词、英文按单词跳词在左右两个方向都正确。
   通过把 nav.c 里 ComputeSteps 改为非 static 来直接调用。
   ============================================================ */
#include "cap.h"
#include "auto.h"
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>

/* nav.c 内部已改为非 static 供测试调用 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir);
/* 单测未链接 main.c，补若干符号 */
volatile BOOL g_recording = FALSE;
HHOOK g_hook = NULL;   /* NavBaseInfo 会读 g_hook，需提供定义 */
void CapLog(const char* fmt, ...) { (void)fmt; }
/* 未链接 hook.c：物理按键状态桩（本测试不涉及卡键清理） */
BOOL PhysKeyDown(int vk) { (void)vk; return FALSE; }
/* 未链接 wps.c：WPS 通道桩（真实现要链接 wps.c + autodisp.c + bridge.c，
   本测试只测词边界算法/其它通道，这里让它"读不到"即可）。
   签名从 src/auto.h 抄，改了接口这里要跟着改 —— 编译器会立刻提醒。 */
BOOL WpsGetTextCaret(HWND focus, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    (void)focus; (void)text; (void)lenOut; (void)caretOut; (void)selEndOut;
    return FALSE;
}
int WpsSetSel(HWND focus, int start, int end) {
    (void)focus; (void)start; (void)end;
    return WPS_SET_NONE;   /* 什么都没做 -> 调用方可安全退回箭头 */
}

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

    /* 中英数字混排：字母数字同组一词（21M 整体），类型切换处停下
       你0好1，2世3界4 1234abcd:5-12 中13国14, len=15 */
    const wchar_t* md = L"你好，世界1234abcd中国";
    R(md, 0, 3, "MD: 头右 0->3(跨 你好，到世界首)");
    R(md, 3, 2, "MD: 世界右 3->5(到 1234abcd 首)");
    R(md, 5, 8, "MD: 1234右 5->13(1234abcd 一词,到中国首)");
    R(md, 9, 4, "MD: abcd右 9->13(到中国首)");
    R(md, 13, 2, "MD: 中国右 13->尾");
    L(md, 15, 2, "MD: 末尾左 15->13(中国词首)");
    L(md, 13, 8, "MD: 中国首左 13->5(跨 1234abcd)");
    L(md, 9, 4, "MD: abcd首左 9->5(跨 abcd)");
    L(md, 5, 2, "MD: 1234首左 5->3(世界)");

    /* 全角字母/数字同组一词 */
    const wchar_t* fc = L"你好Ａ１ｂ２中国";         /* 你0好1 Ａ１ｂ２:2-5 中6国7, len=8 */
    R(fc, 0, 2, "FC: 头右 0->2(跨 你好到Ａ)");
    R(fc, 2, 4, "FC: Ａ右 2->6(全角字母数字一词)");
    R(fc, 3, 3, "FC: １右 3->6");
    R(fc, 4, 2, "FC: ｂ右 4->6");
    R(fc, 5, 1, "FC: ２右 5->6");
    L(fc, 8, 2, "FC: 末尾左 8->6(跨 中国)");
    L(fc, 6, 4, "FC: 中国首左 6->2(跨 Ａ１ｂ２)");

    /* 用户样例：21M 应整体一词（字母数字同组）。
       标点跟随前词一起跨（与 你好，|世界，| 语义一致）：
       右键托盘菜单，|就会升到|21M，|然后始终不回落 */
    const wchar_t* ex = L"升到21M，尾";              /* 升0到1 21M:2-4 ，5 尾6 */
    R(ex, 0, 2, "EX: 头右 0->2(升到|21M)");
    R(ex, 2, 4, "EX: 21M右 2->6(跨 21M，)");
    R(ex, 5, 1, "EX: 逗号右 5->6");
    L(ex, 7, 1, "EX: 末尾左 7->6");
    L(ex, 5, 3, "EX: 逗号前左 5->2(跨 21M,逗号在右侧不参与)");

    /* 行边界：词跳默认不跨行；只有出发点在行尾/行首才跨到相邻行 */
    const wchar_t* ln = L"abc\ndef";                  /* a0b1c2 \n3 d4e5f6 */
    R(ln, 0, 3, "LN: 行首右跳不跨行 -> 3(本行行尾)");
    R(ln, 1, 2, "LN: 行中右跳 -> 3(行尾)");
    R(ln, 3, 1, "LN: 行尾右跳跨行 -> 4(下一行行首)");
    L(ln, 4, 1, "LN: 行首左跳跨行 -> 3(上一行行尾)");
    L(ln, 6, 2, "LN: 行中左跳 6->4(行内词首 de)");
    L(ln, 7, 3, "LN: 行尾左跳 7->4(跨 def)");

    /* 行尾空格：先吃到行尾停下，下一次才跨行 */
    const wchar_t* sp2 = L"abc  \ndef";               /* abc 0-2 空3空4 \n5 def6-8 */
    R(sp2, 0, 5, "LN2: 词+行尾空格 -> 5(停在行尾)");
    R(sp2, 5, 1, "LN2: 行尾右跳 -> 6(下一行词首)");
    L(sp2, 6, 3, "LN2: 行首左跳 -> 3(上一行词尾,跳过缩进)");

    /* 空行：跨行时跳过空行，停在下一个有内容的词 */
    const wchar_t* el = L"a\n\nb";                    /* a0 \n1 \n2 b3 */
    R(el, 0, 1, "LN3: 行尾右跳 -> 1");
    R(el, 1, 2, "LN3: 跨空行 -> 3(b)");
    L(el, 3, 2, "LN3: 行首左跳跨空行 -> 1(a 词尾)");

    /* 谚文（韩文）自成一组：한국어 一词，与汉字/字母互为边界 */
    const wchar_t* hg = L"한국어abc，한글";            /* 한국어:0-2 abc:3-5 ，6 한글:7-8 */
    R(hg, 0, 3, "HG: 谚文右 0->3(한국어 一词)");
    R(hg, 3, 4, "HG: abc右 3->7(跨 abc，)");
    R(hg, 7, 2, "HG: 한글右 7->9");
    L(hg, 9, 2, "HG: 末尾左 9->7(한글)");
    L(hg, 7, 4, "HG: 한글首左 7->3(跨 ，)");
    L(hg, 3, 3, "HG: abc首左 3->0(跨 abc)");

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}