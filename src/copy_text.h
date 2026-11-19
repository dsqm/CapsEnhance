#ifndef COPY_TEXT_H
#define COPY_TEXT_H

#include <windows.h>

/* ================= 控件复制的纯文本处理（src/copy_text.c） =================
   为什么单独一个 TU：这两件事（清洗、截断长度）是**纯函数**，不碰 COM、
   不碰窗口、不碰配置，可以被单测直接验证（与 nav_word.c 同一个理由：算法
   与取词/画图分开，测试才不需要拉起整个无障碍树）。

   ★ 两处都用同一份实现，是为了让"浮层显示多少"与"剪贴板里存多少"永远
     基于同一套换行语义 —— 一边按 \r\n 数、另一边按 \n 数，行数就会对不上。 */

/* 统一换行（CRLF / 裸 CR -> LF），可选去掉**尾部**空白与换行（\n \t \v \f 空格）。
   **内部换行与缩进原样保留**：多行文本（一段代码、几行日志）是真实场景。
   out 的容量 cap 以字符计（含结尾 0）；返回清洗后的长度（不含结尾 0）。
   截断只可能发生在 out 容量不足时，正常调用方给 len+1 即可。 */
int CopyClean(const WCHAR* src, int len, BOOL stripTail, WCHAR* out, int cap);

/* 按"先按行、再按字"算出**预览**应当显示的字符数（不含省略号）。
   行数优先：超过 maxLines 行就在第 maxLines 行末截断；否则超过 maxChars
   字就截到 maxChars。*truncated 表示是否真的被截（调用方据此追加省略号）。
   ★ 只服务显示：剪贴板永远写全文。 */
int CopyPreviewLen(const WCHAR* src, int len, int maxChars, int maxLines, BOOL* truncated);

#endif /* COPY_TEXT_H */
