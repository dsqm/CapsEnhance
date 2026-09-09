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
/* 单测里 CapLog 无真实日志目标，做成空桩 */
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
    L(en, 6, 6,  "EN: world起 左 6->0(跨 world+空格)");

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}