/* ================= 控件复制的纯文本处理 =================
   见 copy_text.h 的方向说明。本文件刻意不含任何 cap.h / COM / 配置依赖：
   它要能被单测（test/test_copytext.c）直接 #include 进来验证。 */

#include "copy_text.h"

#include <wchar.h>

int CopyClean(const WCHAR* src, int len, BOOL stripTail, WCHAR* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!src || len <= 0) { out[0] = 0; return 0; }

    int o = 0;
    for (int i = 0; i < len; i++) {
        if (o >= cap - 1) break;
        WCHAR c = src[i];
        if (c == L'\r') {
            /* CRLF 的 CR 直接丢掉（下一轮的 LF 会成为唯一换行符）；
               裸 CR（老 Mac 风格 / 某些控件）换成 LF。 */
            if (i + 1 < len && src[i + 1] == L'\n') continue;
            c = L'\n';
        }
        out[o++] = c;
    }
    if (stripTail) {
        while (o > 0) {
            WCHAR c = out[o - 1];
            if (c == L'\n' || c == L' ' || c == L'\t' || c == L'\v' || c == L'\f') o--;
            else break;
        }
    }
    out[o] = 0;
    return o;
}

int CopyPreviewLen(const WCHAR* src, int len, int maxChars, int maxLines, BOOL* truncated) {
    if (truncated) *truncated = FALSE;
    if (!src || len <= 0) return 0;
    if (maxChars < 1) maxChars = 1;
    if (maxLines < 1) maxLines = 1;

    int cut = len;
    int lines = 1;
    for (int i = 0; i < len; i++) {
        if (src[i] == L'\n') {
            lines++;
            if (lines > maxLines) { cut = i; break; }
        }
    }
    if (cut > maxChars) cut = maxChars;

    if (truncated) *truncated = (cut < len);
    return cut;
}
