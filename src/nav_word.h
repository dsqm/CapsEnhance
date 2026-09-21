#ifndef NAV_WORD_H
#define NAV_WORD_H

/* 用聚合头引入：本文件要用 cap.h 里的 DIR_LEFT / DIR_RIGHT，而 cap.h 又是
   "所有 TU 的第一个包含"（它必须早于任何 SDK 头，见其顶部）。cap.h 自带
   包含保护，nav.c 之后再包含它是空操作。 */
#include "cap.h"

/* ================= 词边界算法（src/nav_word.c） =================
   按词跳转的**纯**部分：字符分类 + 代理对处理 + "往某方向走一步的落点"。
   整段不碰 HWND / COM / 任何跨进程调用，也不读配置 —— 所以它能被单独
   编译、单独读、单独测（test/test_nav.c 直接对着 ComputeSteps 断言）。

   为什么值得从 nav.c 里分出来：nav.c 其余部分全是"读文本 + 置光标"的通道
   （Edit / Scintilla / UIA / IA2 / WPS）与跨进程限时，读它们必须带一堆
   COM 前置知识；而这段是纯算法，回答问题只需要知道"什么是词"。 */

/* 从 s[caret] 出发，朝 dir（DIR_LEFT / DIR_RIGHT）跳一次，返回新位置，
   并把"移动了几个字符"写进 *steps（词算法用 steps 决定逐格还是瞬跳）。
   字符串两端即停（返回原位）。 */
int WordJumpTarget(const WCHAR* s, int len, int caret, int dir, int* steps);

/* 同上的薄包装：只要步数（cap.h 里对外声明，供单测与诊断用）。 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir);

#endif /* NAV_WORD_H */
