/* ======================================================================
   CapsEnhance —— Windows CapsLock 快捷键增强程序
   Copyright (C) 2026  qm

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
   ------------------------------------------------------------------ */

#include "nav_word.h"

/* 代理对判定：补充平面字符用两个 UTF-16 code unit 表示（高+低代理），
   必须当作"一个字符"整体推进，否则会把它们拆散导致按词跳转错乱。 */
static BOOL IsHighSurrogateW(WCHAR c) { return c >= 0xD800 && c <= 0xDBFF; }
static BOOL IsLowSurrogateW(WCHAR c)  { return c >= 0xDC00 && c <= 0xDFFF; }

/* 字符类别：决定按词跳转边界。连续同类字符串视为"一个词"，
   按词跳转就停在这些词(与词/分隔符)的边界处。
   英文与数字同组（21M 是一个词）；谚文（韩文）自成一组；
   采用"反向"判定：先把 字母数字/谚文/符号 归类，
   其余兜底一律算 CH_OTHER（汉字、其他语言字符、生僻字、emoji 等）。 */
typedef enum { CH_OTHER, CH_HANGUL, CH_ALPHA, CH_SEP } CharClass;

static CharClass ClassOf(WCHAR c) {
    unsigned cp = (unsigned)c;
    /* 字母+数字（含全角 Ａ-Ｚ ａ-ｚ ０-９）同属一组 */
    if ((cp >= 0x30 && cp <= 0x39) || (cp >= 0x41 && cp <= 0x5A) ||
        (cp >= 0x61 && cp <= 0x7A))
        return CH_ALPHA;
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
        (cp >= 0xFF41 && cp <= 0xFF5A))
        return CH_ALPHA;
    /* 谚文（韩文）自成一组，完全按指定范围匹配：
       Hangul Jamo / 兼容 Jamo / Jamo Extended-A / 音节 */
    if ((cp >= 0x1100 && cp <= 0x11FF) ||
        (cp >= 0x3130 && cp <= 0x318F) ||
        (cp >= 0xA960 && cp <= 0xA97F) ||
        (cp >= 0xAC00 && cp <= 0xD7FF))
        return CH_HANGUL;
    /* 分隔符：空白 + 标点 */
    if (c == L'\0') return CH_SEP;
    if (iswspace(c)) return CH_SEP;
    if (iswpunct(c)) return CH_SEP;
    if (cp >= 0x2000 && cp <= 0x206F) return CH_SEP;  /* 通用标点 */
    if (cp >= 0x3000 && cp <= 0x303F) return CH_SEP;  /* CJK 标点(含全角空格) */
    if (cp >= 0xFF00 && cp <= 0xFFEF) return CH_SEP;  /* 其余全角符号标点 */
    /* 兜底：纯反向判定——非英文/数字/符号，一律归"其他"(含汉字与其他语言字符) */
    return CH_OTHER;
}

/* 以 s[i] 为"字符起点"（可能是一个代理对的首字）分类。 */
static CharClass ClassOfAt(const WCHAR* s, int len, int i) {
    if (i < 0 || i >= len) return CH_SEP;
    if (i + 1 < len && IsHighSurrogateW(s[i]) && IsLowSurrogateW(s[i+1]))
        return CH_OTHER;                               /* 补充平面字符：生僻字/emoji 等 → 其他 */
    if (IsHighSurrogateW(s[i]) || IsLowSurrogateW(s[i]))
        return CH_SEP;                                 /* 孤立半代理按分隔 */
    return ClassOf(s[i]);
}

/* 由 code-unit 位置 i 反推其左侧那个"字符"的起点（代理对整体归为一句）。 */
static int CharStartBefore(const WCHAR* s, int len, int i) {
    (void)len;
    if (i <= 0) return 0;
    /* 前一 code unit 是 low 代理，则它属于 high 起头的那个字符 */
    if (IsLowSurrogateW(s[i-1]) && i >= 2 && IsHighSurrogateW(s[i-2]))
        return i - 2;
    return i - 1;
}

/* 按词跳转核心：返回跳转后光标应处的 code-unit 位置，同时写出"需发送的箭头次数"。
   左右箭头在编辑框内按"字符"（code point）移动，因此步数(箭头数)按字符单位计数，
   代理对整体只计 1 步；目标 code-unit 位置供经典 Edit 直接置光标(瞬跳)。
   步数由 target 与 caret 的字符数差得到，与逐箭头移动完全等价。

   行边界规则：按词跳转默认不跨行（\r/\n 是分隔符，但分隔符串只在行内跨）。
   只有出发点已经在行尾时，右跳才跨过换行落到下一行行首词；
   只有出发点已经在行首时，左跳才跨过换行落到上一行行尾词。 */
int WordJumpTarget(const WCHAR* s, int len, int caret, int dir, int* steps) {
    *steps = 0;
    if (!s || len < 1) return caret;
    /* 归一光标：落在代理对中间(异常，不该发生)时，视为整个字符未跨过 */
    if (caret > 0 && caret < len && IsHighSurrogateW(s[caret-1]) && IsLowSurrogateW(s[caret]))
        caret--;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;

    int i = caret, st = 0;

    if (dir == DIR_RIGHT) {
        int atEOL = (i >= len || s[i] == L'\n' || s[i] == L'\r');
        /* 跨当前同类型词串（\n 是分隔符，词串自然停在行尾） */
        CharClass cur = ClassOfAt(s, len, i);
        if (cur != CH_SEP) {
            while (i < len) {
                if (ClassOfAt(s, len, i) != cur) break;
                st++;
                i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
            }
        }
        /* 跨本行右侧分隔符（空格/制表等，不含换行） */
        while (i < len && ClassOfAt(s, len, i) == CH_SEP &&
               s[i] != L'\n' && s[i] != L'\r') {
            st++;
            i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
        }
        /* 出发点就在行尾：跨过换行与下一行行首缩进，停在下一行词首 */
        if (atEOL) {
            while (i < len && ClassOfAt(s, len, i) == CH_SEP) {
                st++;
                i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
            }
        }
    } else {
        int atBOL = (i <= 0 || s[i-1] == L'\n' || s[i-1] == L'\r');
        /* 跨本行左侧分隔符（空格/制表等，不含换行） */
        while (i > 0) {
            int st0 = CharStartBefore(s, len, i);
            if (ClassOfAt(s, len, st0) != CH_SEP) break;
            if (s[st0] == L'\n' || s[st0] == L'\r') break;
            st++; i = st0;
        }
        if (atBOL && i > 0) {
            /* 出发点就在行首：跨过换行与上一行行尾缩进，停在上一行词尾 */
            while (i > 0) {
                int st0 = CharStartBefore(s, len, i);
                if (ClassOfAt(s, len, st0) != CH_SEP) break;
                st++; i = st0;
            }
        } else if (i > 0) {
            /* 跨到左侧同类型词串首 */
            CharClass cur = ClassOfAt(s, len, CharStartBefore(s, len, i));
            while (i > 0) {
                int st0 = CharStartBefore(s, len, i);
                if (ClassOfAt(s, len, st0) != cur) break;
                st++; i = st0;
            }
        }
    }
    *steps = st;
    return i;
}

/* 非 static 仅供按词跳转单元测试（test/test_nav.c）直接调用。
   声明在 cap.h 里（不要在每个测试文件里各自 extern —— 签名一改就会漏改一处，
   而漏改的后果是链接期才发现，或者更糟：隐式声明按 int 返回值调用）。 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir) {
    int steps = 0;
    WordJumpTarget(s, len, caret, dir, &steps);
    return steps;
}
