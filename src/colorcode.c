/* ============================================================
   colorcode.c — 剪贴板条目的"色值"识别（照 Ditto 的
   CQListCtrl::DrawCopiedColorCode 的判据实现）

   为什么要单独成模块：判据分支多（命名色 / #hex / 0x / rgb() / hsl() /
   括号 RGB / 逗号 RGB / 空格 RGB / 6 位纯 hex），且**纯函数无 Win32 依赖**，
   因此可以像 nav.c 的 ComputeSteps 那样进单测（test/test_color.c）。

   来源：Ditto `src/QListCtrl.cpp` 的 DrawCopiedColorCode() 与
   InitializeColorNameMap()（判据与色值表照抄，oklch() 未实现）。
   ============================================================ */
#include "colorcode.h"
#include <math.h>     /* fmod */

/* ---- W3C 命名色（141 个，按名字升序；二分查找） ----
   从 Ditto 的 InitializeColorNameMap() 逐条取出，未作增删。 */
typedef struct { const WCHAR* name; COLORREF rgb; } NamedColor;
static const NamedColor kNamed[] = {
    { L"aliceblue", RGB(240, 248, 255) },
    { L"antiquewhite", RGB(250, 235, 215) },
    { L"aqua", RGB(0, 255, 255) },
    { L"aquamarine", RGB(127, 255, 212) },
    { L"azure", RGB(240, 255, 255) },
    { L"beige", RGB(245, 245, 220) },
    { L"bisque", RGB(255, 228, 196) },
    { L"black", RGB(0, 0, 0) },
    { L"blanchedalmond", RGB(255, 235, 205) },
    { L"blue", RGB(0, 0, 255) },
    { L"blueviolet", RGB(138, 43, 226) },
    { L"brown", RGB(165, 42, 42) },
    { L"burlywood", RGB(222, 184, 135) },
    { L"cadetblue", RGB(95, 158, 160) },
    { L"chartreuse", RGB(127, 255, 0) },
    { L"chocolate", RGB(210, 105, 30) },
    { L"coral", RGB(255, 127, 80) },
    { L"cornflowerblue", RGB(100, 149, 237) },
    { L"cornsilk", RGB(255, 248, 220) },
    { L"crimson", RGB(220, 20, 60) },
    { L"cyan", RGB(0, 255, 255) },
    { L"darkblue", RGB(0, 0, 139) },
    { L"darkcyan", RGB(0, 139, 139) },
    { L"darkgoldenrod", RGB(184, 134, 11) },
    { L"darkgray", RGB(169, 169, 169) },
    { L"darkgreen", RGB(0, 100, 0) },
    { L"darkkhaki", RGB(189, 183, 107) },
    { L"darkmagenta", RGB(139, 0, 139) },
    { L"darkolivegreen", RGB(85, 107, 47) },
    { L"darkorange", RGB(255, 140, 0) },
    { L"darkorchid", RGB(153, 50, 204) },
    { L"darkred", RGB(139, 0, 0) },
    { L"darksalmon", RGB(233, 150, 122) },
    { L"darkseagreen", RGB(143, 188, 143) },
    { L"darkslateblue", RGB(72, 61, 139) },
    { L"darkslategray", RGB(47, 79, 79) },
    { L"darkturquoise", RGB(0, 206, 209) },
    { L"darkviolet", RGB(148, 0, 211) },
    { L"deeppink", RGB(255, 20, 147) },
    { L"deepskyblue", RGB(0, 191, 255) },
    { L"dimgray", RGB(105, 105, 105) },
    { L"dodgerblue", RGB(30, 144, 255) },
    { L"firebrick", RGB(178, 34, 34) },
    { L"floralwhite", RGB(255, 250, 240) },
    { L"forestgreen", RGB(34, 139, 34) },
    { L"fuchsia", RGB(255, 0, 255) },
    { L"gainsboro", RGB(220, 220, 220) },
    { L"ghostwhite", RGB(248, 248, 255) },
    { L"gold", RGB(255, 215, 0) },
    { L"goldenrod", RGB(218, 165, 32) },
    { L"gray", RGB(128, 128, 128) },
    { L"green", RGB(0, 128, 0) },
    { L"greenyellow", RGB(173, 255, 47) },
    { L"honeydew", RGB(240, 255, 240) },
    { L"hotpink", RGB(255, 105, 180) },
    { L"indianred", RGB(205, 92, 92) },
    { L"indigo", RGB(75, 0, 130) },
    { L"ivory", RGB(255, 255, 240) },
    { L"khaki", RGB(240, 230, 140) },
    { L"lavender", RGB(230, 230, 250) },
    { L"lavenderblush", RGB(255, 240, 245) },
    { L"lawngreen", RGB(124, 252, 0) },
    { L"lemonchiffon", RGB(255, 250, 205) },
    { L"lightblue", RGB(173, 216, 230) },
    { L"lightcoral", RGB(240, 128, 128) },
    { L"lightcyan", RGB(224, 255, 255) },
    { L"lightgoldenrodyellow", RGB(250, 250, 210) },
    { L"lightgray", RGB(211, 211, 211) },
    { L"lightgreen", RGB(144, 238, 144) },
    { L"lightpink", RGB(255, 182, 193) },
    { L"lightsalmon", RGB(255, 160, 122) },
    { L"lightseagreen", RGB(32, 178, 170) },
    { L"lightskyblue", RGB(135, 206, 250) },
    { L"lightslategray", RGB(119, 136, 153) },
    { L"lightsteelblue", RGB(176, 196, 222) },
    { L"lightyellow", RGB(255, 255, 224) },
    { L"lime", RGB(0, 255, 0) },
    { L"limegreen", RGB(50, 205, 50) },
    { L"linen", RGB(250, 240, 230) },
    { L"magenta", RGB(255, 0, 255) },
    { L"maroon", RGB(128, 0, 0) },
    { L"mediumaquamarine", RGB(102, 205, 170) },
    { L"mediumblue", RGB(0, 0, 205) },
    { L"mediumorchid", RGB(186, 85, 211) },
    { L"mediumpurple", RGB(147, 112, 219) },
    { L"mediumseagreen", RGB(60, 179, 113) },
    { L"mediumslateblue", RGB(123, 104, 238) },
    { L"mediumspringgreen", RGB(0, 250, 154) },
    { L"mediumturquoise", RGB(72, 209, 204) },
    { L"mediumvioletred", RGB(199, 21, 133) },
    { L"midnightblue", RGB(25, 25, 112) },
    { L"mintcream", RGB(245, 255, 250) },
    { L"mistyrose", RGB(255, 228, 225) },
    { L"moccasin", RGB(255, 228, 181) },
    { L"navajowhite", RGB(255, 222, 173) },
    { L"navy", RGB(0, 0, 128) },
    { L"oldlace", RGB(253, 245, 230) },
    { L"olive", RGB(128, 128, 0) },
    { L"olivedrab", RGB(107, 142, 35) },
    { L"orange", RGB(255, 165, 0) },
    { L"orangered", RGB(255, 69, 0) },
    { L"orchid", RGB(218, 112, 214) },
    { L"palegoldenrod", RGB(238, 232, 170) },
    { L"palegreen", RGB(152, 251, 152) },
    { L"paleturquoise", RGB(175, 238, 238) },
    { L"palevioletred", RGB(219, 112, 147) },
    { L"papayawhip", RGB(255, 239, 213) },
    { L"peachpuff", RGB(255, 218, 185) },
    { L"peru", RGB(205, 133, 63) },
    { L"pink", RGB(255, 192, 203) },
    { L"plum", RGB(221, 160, 221) },
    { L"powderblue", RGB(176, 224, 230) },
    { L"purple", RGB(128, 0, 128) },
    { L"rebeccapurple", RGB(102, 51, 153) },
    { L"red", RGB(255, 0, 0) },
    { L"rosybrown", RGB(188, 143, 143) },
    { L"royalblue", RGB(65, 105, 225) },
    { L"saddlebrown", RGB(139, 69, 19) },
    { L"salmon", RGB(250, 128, 114) },
    { L"sandybrown", RGB(244, 164, 96) },
    { L"seagreen", RGB(46, 139, 87) },
    { L"seashell", RGB(255, 245, 238) },
    { L"sienna", RGB(160, 82, 45) },
    { L"silver", RGB(192, 192, 192) },
    { L"skyblue", RGB(135, 206, 235) },
    { L"slateblue", RGB(106, 90, 205) },
    { L"slategray", RGB(112, 128, 144) },
    { L"snow", RGB(255, 250, 250) },
    { L"springgreen", RGB(0, 255, 127) },
    { L"steelblue", RGB(70, 130, 180) },
    { L"tan", RGB(210, 180, 140) },
    { L"teal", RGB(0, 128, 128) },
    { L"thistle", RGB(216, 191, 216) },
    { L"tomato", RGB(255, 99, 71) },
    { L"turquoise", RGB(64, 224, 208) },
    { L"violet", RGB(238, 130, 238) },
    { L"wheat", RGB(245, 222, 179) },
    { L"white", RGB(255, 255, 255) },
    { L"whitesmoke", RGB(245, 245, 245) },
    { L"yellow", RGB(255, 255, 0) },
    { L"yellowgreen", RGB(154, 205, 50) },
};

/* ---- 解析逻辑 ----
   判据与顺序照抄 Ditto，一处例外：`oklch()` 未实现（罕见，且要搬 OKLab→sRGB
   那一整套矩阵）。它命中不了时就是普通文本，不会误伤。 */

#define CC_BUF 64          /* 色值表达式最长也就 "rgba(255, 255, 255, 0.5)" 这种量级 */

static int HexVal(WCHAR c) {
    if (c >= L'0' && c <= L'9') return (int)(c - L'0');
    if (c >= L'a' && c <= L'f') return (int)(c - L'a') + 10;
    if (c >= L'A' && c <= L'F') return (int)(c - L'A') + 10;
    return -1;
}

static BOOL IsHexAll(const WCHAR* s, int len) {
    if (len <= 0) return FALSE;
    for (int i = 0; i < len; i++) if (HexVal(s[i]) < 0) return FALSE;
    return TRUE;
}

static BOOL IsSp(WCHAR c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

/* 照 Ditto 的 ParseCssValue：宽容 —— 只要开头能读出一个数就算成功，
   尾部残留（"deg" 之类）不管。*isPct 报告是不是百分比。 */
static BOOL NumOf(const WCHAR* tok, double* out, BOOL* isPct) {
    while (IsSp(*tok)) tok++;
    if (*tok == 0) return FALSE;
    WCHAR* end = NULL;
    double v = wcstod(tok, &end);
    if (end == tok) return FALSE;
    if (isPct) *isPct = FALSE;
    /* 只看最后一个字符是不是 '%'（Ditto 用 Right(1)） */
    size_t n = wcslen(tok);
    if (n > 0 && tok[n - 1] == L'%') {
        if (isPct) *isPct = TRUE;
    }
    *out = v;
    return TRUE;
}

/* 按空格切分（连续的空白当一个分隔符），最多 maxTok 个；返回实际个数 */
static int SplitTokens(const WCHAR* s, WCHAR out[][16], int maxTok) {
    int cnt = 0;
    while (*s) {
        while (IsSp(*s)) s++;
        if (*s == 0) break;
        if (cnt >= maxTok) return cnt;
        int k = 0;
        while (*s && !IsSp(*s) && k < 15) out[cnt][k++] = *s++;
        out[cnt][k] = 0;
        cnt++;
    }
    return cnt;
}

static int Clamp255(double v) {
    if (v < 0.0)   v = 0.0;
    if (v > 255.0) v = 255.0;
    return (int)(v + 0.5);
}

static double Hue2rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5)        return q;
    if (t < 2.0 / 3.0)  return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

static COLORREF HslToRgb(double h, double s, double l) {
    double r, g, b;
    if (s == 0.0) {
        r = g = b = l;
    } else {
        double q = (l < 0.5) ? l * (1.0 + s) : l + s - l * s;
        double p = 2.0 * l - q;
        double hn = h / 360.0;
        r = Hue2rgb(p, q, hn + 1.0 / 3.0);
        g = Hue2rgb(p, q, hn);
        b = Hue2rgb(p, q, hn - 1.0 / 3.0);
    }
    return RGB(Clamp255(r * 255.0), Clamp255(g * 255.0), Clamp255(b * 255.0));
}

/* W3C 命名色（表已按名字升序 -> 二分） */
static BOOL NamedColorOf(const WCHAR* low, COLORREF* rgb) {
    int lo = 0, hi = (int)(sizeof(kNamed) / sizeof(kNamed[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = wcscmp(low, kNamed[mid].name);
        if (c == 0) { *rgb = kNamed[mid].rgb; return TRUE; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return FALSE;
}

BOOL CcParseColor(const WCHAR* text, COLORREF* rgbOut, int* alphaOut) {
    if (!text || !rgbOut || !alphaOut) return FALSE;

    /* 1) 清理：首尾的 »、空白、以及右侧的分号（照 Ditto 的顺序） */
    WCHAR buf[CC_BUF];
    int n = (int)wcslen(text);
    if (n <= 0) return FALSE;
    int a = 0, b = n - 1;
    while (a <= b && (text[a] == 0x00BB || IsSp(text[a]))) a++;
    while (b >= a && (text[b] == 0x00BB || IsSp(text[b]))) b--;
    while (b >= a && text[b] == L';') b--;
    while (b >= a && IsSp(text[b])) b--;
    if (b < a) return FALSE;
    n = b - a + 1;
    if (n >= CC_BUF) return FALSE;          /* 长到这个份上不可能是纯色值 */
    /* 逐元素拷贝（不走 memcpy）：/analyze 会把 memcpy 建模成
       "buf 可读范围恰为 n 个 WCHAR"，之后任何按 buf[i] 的访问都报 C6385。 */
    for (int i = 0; i < n; i++) buf[i] = text[a + i];
    buf[n] = 0;

    /* 小写副本：命名色与前缀判定都按它比（hex 本身不区分大小写） */
    WCHAR low[CC_BUF];
    for (int i = 0; i < n; i++)
        low[i] = (buf[i] >= L'A' && buf[i] <= L'Z') ? (WCHAR)(buf[i] + 32) : buf[i];
    low[n] = 0;

    /* 2) W3C 命名色 */
    COLORREF named = 0;
    if (NamedColorOf(low, &named)) { *rgbOut = named; *alphaOut = 255; return TRUE; }

    /* 3) # / 0x 前缀的 hex：3、4 位简写先展开，6 = RGB，8 = RGBA */
    {
        const WCHAR* hx = NULL;
        if (low[0] == L'#') hx = low + 1;
        else if (low[0] == L'0' && low[1] == L'x') hx = low + 2;
        if (hx) {
            int hl = (int)wcslen(hx);
            WCHAR ex[12];
            if (hl == 3 || hl == 4) {                 /* #RGB / #RGBA -> 每位重复 */
                int k = 0;
                for (int i = 0; i < hl && k < 8; i++) { ex[k++] = hx[i]; ex[k++] = hx[i]; }
                ex[k] = 0;
                hx = ex;
                hl = k;
            }
            if ((hl == 6 || hl == 8) && IsHexAll(hx, hl)) {
                int r = HexVal(hx[0]) * 16 + HexVal(hx[1]);
                int g = HexVal(hx[2]) * 16 + HexVal(hx[3]);
                int bl = HexVal(hx[4]) * 16 + HexVal(hx[5]);
                int al = 255;
                if (hl == 8) al = HexVal(hx[6]) * 16 + HexVal(hx[7]);
                *rgbOut = RGB(Clamp255((double)r), Clamp255((double)g), Clamp255((double)bl));
                *alphaOut = (al < 0) ? 0 : (al > 255 ? 255 : al);
                return TRUE;
            }
            return FALSE;                              /* 有前缀就不该再往下猜 */
        }
    }

    /* 4) rgb() / hsl() 这类带括号的 CSS 记法 */
    if (low[n - 1] == L')') {
        if (wcsncmp(low, L"rgb", 3) == 0 || wcsncmp(low, L"hsl", 3) == 0) {
            BOOL isHsl = (wcsncmp(low, L"hsl", 3) == 0);
            int plen = (low[3] == L'a') ? 5 : 4;        /* rgba( / hsla( -> 5 */
            if (n > plen + 1) {
                WCHAR tmp[CC_BUF];
                int tl = n - plen - 1;                  /* 去掉前缀与右括号 */
                memcpy(tmp, low + plen, (size_t)tl * sizeof(WCHAR));
                tmp[tl] = 0;
                for (int i = 0; i < tl; i++) {
                    if (tmp[i] == L',' || tmp[i] == L'/') tmp[i] = L' ';
                }
                /* hsl 的色相可能写成 120deg：照 Ditto 先把 "deg" 抹掉 */
                if (isHsl) {
                    WCHAR deg[CC_BUF];
                    int k = 0;
                    for (int i = 0; i < tl; ) {
                        if (wcsncmp(tmp + i, L"deg", 3) == 0) { i += 3; continue; }
                        deg[k++] = tmp[i++];
                    }
                    deg[k] = 0;
                    memcpy(tmp, deg, (size_t)(k + 1) * sizeof(WCHAR));
                }
                WCHAR tok[8][16];
                int cnt = SplitTokens(tmp, tok, 8);
                if (cnt >= 3) {
                    double v0 = 0, v1 = 0, v2 = 0, va = 1.0;
                    BOOL p0 = FALSE, p3 = FALSE;
                    if (!NumOf(tok[0], &v0, &p0)) return FALSE;
                    if (!NumOf(tok[1], &v1, NULL)) return FALSE;
                    if (!NumOf(tok[2], &v2, NULL)) return FALSE;
                    int alpha = 255;
                    if (cnt >= 4 && NumOf(tok[3], &va, &p3)) {
                        if (p3) va /= 100.0;
                        if (va < 0.0) va = 0.0;
                        if (va > 1.0) va = 1.0;
                        alpha = (int)(va * 255.0 + 0.5);
                    }
                    if (isHsl) {
                        if (v1 < 0.0 || v1 > 100.0 || v2 < 0.0 || v2 > 100.0) return FALSE;
                        if (v0 < 0.0) { v0 = fmod(v0, 360.0) + 360.0; }
                        v0 = fmod(v0, 360.0);
                        *rgbOut = HslToRgb(v0, v1 / 100.0, v2 / 100.0);
                        *alphaOut = alpha;
                        return TRUE;
                    } else {
                        if (p0) { v0 *= 2.55; v1 *= 2.55; v2 *= 2.55; }   /* 照 Ditto：只看第一个分量 */
                        if (v0 < 0.0 || v0 > 255.0 || v1 < 0.0 || v1 > 255.0 ||
                            v2 < 0.0 || v2 > 255.0) return FALSE;
                        *rgbOut = RGB(Clamp255(v0), Clamp255(v1), Clamp255(v2));
                        *alphaOut = alpha;
                        return TRUE;
                    }
                }
            }
        }
    }

    /* 5) 剩下的"三个 0..255 整数"形式：
         (255, 128, 0) / 255, 0, 0 / 255 128 0 —— 括号、逗号、空格三种分隔，
         Ditto 是三段几乎一样的代码，这里合并成一段（判据等价）。 */
    {
        WCHAR tmp[CC_BUF];
        int tl = 0;
        for (int i = 0; i < n; i++) {
            WCHAR c = low[i];
            if (c == L'(' || c == L')' || c == L',') c = L' ';
            tmp[tl++] = c;
        }
        tmp[tl] = 0;
        WCHAR tok[8][16];
        int cnt = SplitTokens(tmp, tok, 8);
        if (cnt == 3) {
            double v0 = 0, v1 = 0, v2 = 0;
            BOOL p0 = FALSE, p1 = FALSE, p2 = FALSE;
            if (!NumOf(tok[0], &v0, &p0) || p0) return FALSE;   /* 带 % 的不算整数 RGB */
            if (!NumOf(tok[1], &v1, &p1) || p1) return FALSE;
            if (!NumOf(tok[2], &v2, &p2) || p2) return FALSE;
            if (v0 < 0 || v0 > 255 || v1 < 0 || v1 > 255 || v2 < 0 || v2 > 255) return FALSE;
            if (v0 != (double)(int)v0 || v1 != (double)(int)v1 || v2 != (double)(int)v2) return FALSE;
            *rgbOut = RGB(Clamp255(v0), Clamp255(v1), Clamp255(v2));
            *alphaOut = 255;
            return TRUE;
        }
    }

    /* 6) 6 位纯 hex（没有 # 前缀，例如 F45F01） */
    if (n == 6 && IsHexAll(low, 6)) {
        int r = HexVal(low[0]) * 16 + HexVal(low[1]);
        int g = HexVal(low[2]) * 16 + HexVal(low[3]);
        int bl = HexVal(low[4]) * 16 + HexVal(low[5]);
        *rgbOut = RGB(r, g, bl);
        *alphaOut = 255;
        return TRUE;
    }

    return FALSE;
}
