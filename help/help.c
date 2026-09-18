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

   ------------------------------------------------------------------
   分层窗口（半透明浮层）的实现思路取自本机的 IMEIndicator 项目
     https://github.com/  （同作者，GPL-3.0-or-later）
   那里也是 `WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` + 32bpp DIB
   Section + `UpdateLayeredWindow(ULW_ALPHA)`，本文件沿用同一套。
   键盘布局的键位宽度/间隔按 ANSI 104 标准（15u 主键区、功能行 Esc 与 F1 之间
   隔 1u、F 键每四个一组各隔 0.5u），取自 keyboard-layout-editor 的 ANSI-104 定义。
   ------------------------------------------------------------------ */

#include "cap.h"
#include "keymap_table.h"   /* 键位表：**不抄一份**，与主程序共用同一份定义 */

/* GDI+ 的 SDK 头在 `/analyze` 下会吐几条**它自己**的告警（`GdiplusFontCollection.h`
   的 C6385 与 C28252/C28253）。那是系统头的问题，不是本文件的，而本仓库的规矩是
   `/analyze` 零告警 —— 所以在这几行 include 外面精确压掉，`pop` 之后立刻恢复，
   本文件自己触发的同名告警照旧会被报出来。 */
#pragma warning(push)
#pragma warning(disable: 6385 28252 28253)
#include <objidl.h>
#include <gdiplus.h>
#pragma warning(pop)
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

/* ================= 按键帮助窗口（CapsEnhanceHelp.exe） =================
   它是什么：按住 `Caps + F1` 时**不抢焦点**地盖在屏幕上的一张半透明键位图 —— 
   键帽上印着功能（J 上印 `J`、下印 `←`；D 上印 `D`、下印 `增强`），底部两栏
   列出不是"某个键"的那几条功能（滚轮三档 / 单击切输入法 / 左键拖动）。

   三个硬约束与它们的落地方式：
     1. **不抢焦点**。窗口带 `WS_EX_NOACTIVATE`，显示用 `SW_SHOWNOACTIVATE` —— 
        全程不激活、不改变前台窗口，所以可以"一边按着 C+F1 看，一边继续在
        原来的程序里操作"。也因此**不能用 Esc 关闭**（没有焦点就收不到它）。
     2. **半透明且好看**。用逐像素 alpha（32bpp DIB + `UpdateLayeredWindow`）：
        背板半透明、键帽与文字完全实心且抗锯齿 —— 比"整窗一起调透明度"干净
        （后者连文字一起发虚）。文字用 **GDI+ 的灰阶抗锯齿**，不上 ClearType：
        见"文字"那一节的实测结论。
     3. **启动要快**。自包含：不读配置、不装钩子、不初始化 COM/TSF，只做
        "GDI+ 起一次 + 画一帧 + 送进分层窗口"。键位表直接从共享头编进来，
        没有任何跨进程取数据的往返。

   寿命（由主程序通过注册消息驱动，见 src/help.c）：
     SHOW   -> 显示
     KA     -> 心跳："我还需要你"。800ms 收不到就自己退 —— 这是**唯一的**收尾
               判据：HIDE 丢了、主程序被强杀、主程序卡住，三种情形在别处各有
               各的判据，在这里都是同一件事（喂狗停了）。
     HIDE   -> 隐藏；300ms 后退出（这点缓冲是为了"刚松手又按下"时不来回重启）
   另有一条启动防呆：
     · `--hold` 模式（主程序拉起的那份）下，1.5 秒内没收到任何状态就自己退出 —— 
       主程序把"按下 30ms 就松手"这种情况的 SHOW/HIDE 都发在了窗口出生之前，
       等不到消息说明它早就不需要这个窗口了。没有这条防呆，窗口会永久留在屏幕上。
   不带 `--hold`（用户双击）时是"手动查看"模式：直接显示，点一下关闭，最长也是
   3 分钟 —— 没有人在喂心跳，只能自己兜一个上限。 */

/* ---------- 主题色（#AARRGGBB） ----------
   ★ 浅色，与剪贴板浮层（clip/clip.c）同一套视觉语言：白底、深灰字、
   已映射键浅蓝、修饰键浅琥珀。对比度同样按 WCAG AA（4.5:1）挑：
   浅蓝底 #DCE9F7（L≈0.82）上深字 #1A1A1A 约 14:1，功能名 #1F4E79 约 9:1 ——
   深底浅字时代那种"底色一亮任何浅色都到不了 4.5:1"的约束在浅色下不存在，
   所以这里的取舍简单得多。 */
#define C_PANEL      0xFFFFFFFF   /* 背板：**不透明**。ClearType 必须画在不透明的底上，
                                     半透明的逐像素 alpha 会让它退化成灰阶抗锯齿
                                     （灰阶只有 256 级，横向分辨率是 ClearType 的
                                     三分之一）—— "字发虚"就是从这里来的。
                                     "半透明"改由整窗 LWA_ALPHA 合成期实现（见下），
                                     观感几乎一样，字却是 ClearType 的。 */
#define C_BORDER     0xFFC9C9C9
#define C_TITLE      0xFF1A1A1A
#define C_DIM_FILL   0xFFF5F5F5   /* 未映射的键：留着位置、淡化，不抢注意力 */
#define C_DIM_LINE   0xFFE0E0E0
#define C_DIM_TEXT   0xFF9A9A9A
#define C_ACT_FILL   0xFFDCE9F7   /* 已映射的"动作"键：与剪贴板选中行同款浅蓝 */
#define C_ACT_LINE   0xFF9DC0E8
#define C_ACT_NAME   0xFF1A1A1A
#define C_ACT_FUNC   0xFF1F4E79
#define C_MOD_FILL   0xFFFAEBD5   /* 修饰键（Caps / D / Alt）：浅琥珀，一眼分开 */
#define C_MOD_LINE   0xFFE0B877
#define C_MOD_NAME   0xFF7A5210
#define C_MOD_FUNC   0xFF7A5210
#define C_FOOT_KEY   0xFF2B6CB0
#define C_FOOT_TEXT  0xFF7A7A7A

#define APP_TITLE L"CapsEnhance 按键帮助"
#define KEY_RADIUS_U 0.055f       /* 键帽圆角（单位：u） */
#define CAP_GAP_K    0.05f        /* 键帽左右各留的缝隙（单位 u）：键帽内宽 = (1-它)·u。
                                     这个数收窄是为了**把空间让给字号** —— 灰阶抗锯齿在
                                     小字号下发虚，而字号的上限就是键帽内宽 ÷ 每行字数。 */
#define LABEL_PAD_K  0.03f        /* 标签再往里收一点，别贴到键帽边 */
#define PANEL_RADIUS 16.0f
#define HELP_LIFE_ALPHA 242   /* 整窗不透明度（/255）：95%，与剪贴板浮层一致 */

/* ---------- 键盘布局（单位 u；x/w 按 ANSI 104 实测定义） ----------
   行的竖向位置：功能行占 0..1u，与数字行之间是 **0.25u**（不是普通行距 ——
   这是标准的一部分，全键盘高 6.25u）；数字行往下五行各 1u、行间无额外间隙。 */
#define ROW_COUNT 6
static const float kRowY[ROW_COUNT] = { 0.0f, 1.25f, 2.25f, 3.25f, 4.25f, 5.25f };
#define KBD_UNITS_W 15.0f
#define KBD_UNITS_H 6.25f

typedef struct { int vk; float x; float w; int row; } KeyDef;
static const KeyDef kLayout[] = {
    /* 功能行：Esc 之后隔 1u 才是 F1；F1-F4 / F5-F8 / F9-F12 各隔 0.5u */
    { VK_ESCAPE, 0.0f,  1.0f, 0 },
    { VK_F1,     2.0f,  1.0f, 0 }, { VK_F2, 3.0f, 1.0f, 0 }, { VK_F3, 4.0f, 1.0f, 0 }, { VK_F4, 5.0f, 1.0f, 0 },
    { VK_F5,     6.5f,  1.0f, 0 }, { VK_F6, 7.5f, 1.0f, 0 }, { VK_F7, 8.5f, 1.0f, 0 }, { VK_F8, 9.5f, 1.0f, 0 },
    { VK_F9,    11.0f,  1.0f, 0 }, { VK_F10, 12.0f, 1.0f, 0 }, { VK_F11, 13.0f, 1.0f, 0 }, { VK_F12, 14.0f, 1.0f, 0 },
    /* 数字行 */
    { VK_OEM_3,  0.0f,  1.0f, 1 },
    { '1', 1.0f, 1.0f, 1 }, { '2', 2.0f, 1.0f, 1 }, { '3', 3.0f, 1.0f, 1 }, { '4', 4.0f, 1.0f, 1 },
    { '5', 5.0f, 1.0f, 1 }, { '6', 6.0f, 1.0f, 1 }, { '7', 7.0f, 1.0f, 1 }, { '8', 8.0f, 1.0f, 1 },
    { '9', 9.0f, 1.0f, 1 }, { '0', 10.0f, 1.0f, 1 },
    { VK_OEM_MINUS, 11.0f, 1.0f, 1 }, { VK_OEM_PLUS, 12.0f, 1.0f, 1 },
    { VK_BACK,  13.0f,  2.0f, 1 },
    /* 字母行 */
    { VK_TAB,    0.0f,  1.5f, 2 },
    { 'Q', 1.5f, 1.0f, 2 }, { 'W', 2.5f, 1.0f, 2 }, { 'E', 3.5f, 1.0f, 2 }, { 'R', 4.5f, 1.0f, 2 },
    { 'T', 5.5f, 1.0f, 2 }, { 'Y', 6.5f, 1.0f, 2 }, { 'U', 7.5f, 1.0f, 2 }, { 'I', 8.5f, 1.0f, 2 },
    { 'O', 9.5f, 1.0f, 2 }, { 'P', 10.5f, 1.0f, 2 },
    { VK_OEM_4, 11.5f, 1.0f, 2 }, { VK_OEM_6, 12.5f, 1.0f, 2 },
    { VK_OEM_5, 13.5f, 1.5f, 2 },
    /* 主行 */
    { VK_CAPITAL, 0.0f, 1.75f, 3 },
    { 'A', 1.75f, 1.0f, 3 }, { 'S', 2.75f, 1.0f, 3 }, { 'D', 3.75f, 1.0f, 3 }, { 'F', 4.75f, 1.0f, 3 },
    { 'G', 5.75f, 1.0f, 3 }, { 'H', 6.75f, 1.0f, 3 }, { 'J', 7.75f, 1.0f, 3 }, { 'K', 8.75f, 1.0f, 3 },
    { 'L', 9.75f, 1.0f, 3 },
    { VK_OEM_1, 10.75f, 1.0f, 3 }, { VK_OEM_7, 11.75f, 1.0f, 3 },
    { VK_RETURN, 12.75f, 2.25f, 3 },
    /* 下排 */
    { VK_LSHIFT, 0.0f, 2.25f, 4 },
    { 'Z', 2.25f, 1.0f, 4 }, { 'X', 3.25f, 1.0f, 4 }, { 'C', 4.25f, 1.0f, 4 }, { 'V', 5.25f, 1.0f, 4 },
    { 'B', 6.25f, 1.0f, 4 }, { 'N', 7.25f, 1.0f, 4 }, { 'M', 8.25f, 1.0f, 4 },
    { VK_OEM_COMMA, 9.25f, 1.0f, 4 }, { VK_OEM_PERIOD, 10.25f, 1.0f, 4 }, { VK_OEM_2, 11.25f, 1.0f, 4 },
    { VK_RSHIFT, 12.25f, 2.75f, 4 },
    /* 底行 */
    { VK_LCONTROL, 0.0f, 1.25f, 5 }, { VK_LWIN, 1.25f, 1.25f, 5 }, { VK_LMENU, 2.5f, 1.25f, 5 },
    { VK_SPACE, 3.75f, 6.25f, 5 },
    { VK_RMENU, 10.0f, 1.25f, 5 }, { VK_RWIN, 11.25f, 1.25f, 5 },
    { VK_APPS, 12.5f, 1.25f, 5 }, { VK_RCONTROL, 13.75f, 1.25f, 5 },
};
#define LAYOUT_N ((int)(sizeof(kLayout) / sizeof(kLayout[0])))

/* 底部两栏：**不是"某个键"**的那些功能。放在这里是因为键盘图放不下它们 —— 
   滚轮、单击、鼠标拖动本来就没有对应的键帽。
   ★ 最后三条（单击 Caps / 左 Shift / 右 Shift）的功能名**不是常量**：它们由
   `CapsEnhance.ini` 的 `ImeCapsMode / ImeLShiftMode / ImeRShiftMode` 决定
   （0 不处理 / 1 切中文 / 2 切英文），启动时读出来覆盖 kFootDesc 的默认值 —— 
   写死一个"切中文"在这里，用户把配置一改这张图就开始撒谎。 */
static const char* kFootKey[] = {
    "Caps+滚轮", "Caps+Alt/D+滚轮", "Caps+Shift+滚轮", "Caps+左键拖动",
    "单击 Caps", "单击左 Shift", "单击右 Shift",
};
static const char* kFootDesc[] = {
    "快速滚动", "持续滚动", "横向滚动", "移动窗口",
    "切中文", "切英文", "切英文",   /* ← 后三条会被 ini 里的实际配置覆盖 */
};
#define FOOTER_N 7      /* 条数**写常量**而不是 sizeof 推导：FOOTER_ROWS 要拿它做除法，
                           而静态分析器会把"sizeof(数组)/sizeof(元素)"与紧邻的除法
                           混看成 C6384（Pointer sizeof divided by another value）。
                           下面两条 static_assert 盯着"表改了、这个数没改"。 */
#define FOOTER_COLS 2
#define FOOTER_ROWS ((FOOTER_N + FOOTER_COLS - 1) / FOOTER_COLS)
static_assert(FOOTER_N == _countof(kFootKey),  "FOOTER_N 与 kFootKey 条数不一致");
static_assert(FOOTER_N == _countof(kFootDesc), "FOOTER_N 与 kFootDesc 条数不一致");
static const char* kImeDesc[] = { "不处理", "切中文", "切英文" };

/* ---------- 运行时状态 ---------- */
static HWND  g_hwnd = NULL;
static UINT  g_msgShow = 0, g_msgHide = 0, g_msgKa = 0;
static BOOL  g_holdMode = FALSE;    /* --hold：由主程序驱动（等它给状态） */
static BOOL  g_visible = FALSE;
static ULONGLONG g_lastKa = 0;      /* 最后一次收到心跳的时刻（判据见 TID_KA） */
static int   g_unit = 64;           /* 一个 u 多少像素（按屏幕算，见 FitUnit） */
static int   g_pxW = 0, g_pxH = 0;  /* 整个浮层的像素尺寸 */
/* 布局量：四个"带"的高度与四个字号。**全部由 FitUnit 按 DPI 算一次** —— 
   绘制侧不再自己写第二份常量（曾经"开窗用一份 u、绘制用另一份"就是这么来的）。 */
static float g_pad = 26.0f, g_title = 40.0f, g_gap = 16.0f, g_foot = 84.0f;
static float g_fsName = 12.0f, g_fsFunc = 16.0f, g_fsTitle = 17.0f, g_fsFoot = 13.0f;
static HDC     g_memDC = NULL;
static HBITMAP g_bmp = NULL;
static void*   g_bits = NULL;

#define TID_GRACE  2   /* 300ms：隐完再退，给"又按了一次"留余地 */
#define TID_LIFE   3   /* 3 分钟：**只在手动模式下用**（--hold 有心跳，不需要它） */
#define TID_ARMED  4   /* 1.5s：--hold 下等不到状态就自己退 */
#define TID_KA     5   /* 250ms：查一次心跳是否断供 */
#define GRACE_MS   300
#define LIFE_MS    180000
#define ARMED_MS   1500
#define KA_POLL_MS 250

/* ---------- 小工具 ---------- */

/* UTF-8 -> 宽串（表里的文案是 UTF-8，画之前转一次；失败就当空串）。 */
static void Utf8ToW(const char* s, WCHAR* out, int cap) {
    out[0] = 0;
    if (!s || !*s || cap <= 1) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = 0;
}

/* ---------- 从 CapsEnhance.ini 读那三个"单击切换输入法"的模式 ----------
   为什么要读：这三条的功能名**不是常量** —— 用户可以把它关掉（0）或改方向，
   而图上的文案必须与实际行为一致，否则它就是在撒谎。
   为什么不用 GetPrivateProfileIntW：本 ini 是 **UTF-8（带 BOM）**，而 Windows 的
   profile API 会把带 BOM 的 UTF-8 当成 ANSI 认，BOM 那三个字节会粘在第一行的
   段名前面。这里只关心三个纯 ASCII 的键名与它们的十进制值，按字节扫一遍最省事，
   也不受编码 / BOM / API 缓存那些怪癖影响。（这与"配置解析只有一份实现"不冲突：
   那一条管的是规则的权威性，这里是只读一个显示值、不参与钳制与写回。） */
static int IniReadInt(const char* buf, size_t len, const char* key, int def) {
    size_t kl = strlen(key);
    for (size_t i = 0; i < len; ) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;          /* 本行 = [i, j) */
        size_t s = i;
        while (s < j && (buf[s] == ' ' || buf[s] == '\t')) s++;
        if (s + kl < j && _strnicmp(buf + s, key, kl) == 0) {
            size_t e = s + kl;
            while (e < j && (buf[e] == ' ' || buf[e] == '\t')) e++;
            if (e < j && buf[e] == '=') {
                e++;
                while (e < j && (buf[e] == ' ' || buf[e] == '\t')) e++;
                if (e < j && buf[e] >= '0' && buf[e] <= '9') {
                    int v = 0;
                    while (e < j && buf[e] >= '0' && buf[e] <= '9') {
                        if (v < 100) v = v * 10 + (buf[e] - '0');
                        e++;
                    }
                    return v;
                }
                return def;   /* 键在但值不是数字：与 config.c 同规矩，回落默认 */
            }
        }
        i = (j < len) ? j + 1 : len;
    }
    return def;
}

/* 读三个模式（0 不处理 / 1 切中文 / 2 切英文）；ini 不在或读失败就用默认 1/2/1。 */
static void LoadImeModes(int modes[3]) {
    modes[0] = 1; modes[1] = 2; modes[2] = 1;   /* 与 IME_*_MODE_DEFAULT 同值 */
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return;
    WCHAR* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    size_t off = (size_t)(slash + 1 - path);
    if (off + 16 >= MAX_PATH) return;
    wcscpy_s(slash + 1, MAX_PATH - off, L"CapsEnhance.ini");

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    static char buf[16384];   /* [General] 段就在文件头部，16KB 足够覆盖它 */
    DWORD got = 0;
    if (ReadFile(f, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        modes[0] = IniReadInt(buf, got, "ImeCapsMode",   modes[0]);
        modes[1] = IniReadInt(buf, got, "ImeLShiftMode", modes[1]);
        modes[2] = IniReadInt(buf, got, "ImeRShiftMode", modes[2]);
    }
    CloseHandle(f);
    for (int i = 0; i < 3; i++)
        if (modes[i] > 2) modes[i] = 2;
}

/* 键帽上的**原键名**（J / F1 / Space / ` …）。先查特例表，字母数字直接用字符，
   F1..F12 按序生成。名字始终画在键帽上 —— 功能名只是"加"在它下面，不替代它。 */
static const struct { int vk; const WCHAR* name; } kKeyNames[] = {
    { VK_ESCAPE, L"Esc" }, { VK_BACK, L"Backspace" }, { VK_TAB, L"Tab" },
    { VK_CAPITAL, L"Caps" }, { VK_RETURN, L"Enter" }, { VK_SPACE, L"Space" },
    { VK_LSHIFT, L"Shift" }, { VK_RSHIFT, L"Shift" },
    { VK_LCONTROL, L"Ctrl" }, { VK_RCONTROL, L"Ctrl" },
    { VK_LMENU, L"Alt" }, { VK_RMENU, L"Alt" },
    { VK_LWIN, L"Win" }, { VK_RWIN, L"Win" }, { VK_APPS, L"Menu" },
    { VK_OEM_3, L"`" }, { VK_OEM_MINUS, L"-" }, { VK_OEM_PLUS, L"=" },
    { VK_OEM_4, L"[" }, { VK_OEM_6, L"]" }, { VK_OEM_5, L"\\" },
    { VK_OEM_1, L";" }, { VK_OEM_7, L"'" },
    { VK_OEM_COMMA, L"," }, { VK_OEM_PERIOD, L"." }, { VK_OEM_2, L"/" },
};
static void KeyNameOf(int vk, WCHAR* out, int cap) {
    out[0] = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        _snwprintf_s(out, (size_t)cap, _TRUNCATE, L"%c", (WCHAR)vk);
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        _snwprintf_s(out, (size_t)cap, _TRUNCATE, L"F%d", vk - VK_F1 + 1);
        return;
    }
    for (int i = 0; i < (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0])); i++) {
        if (kKeyNames[i].vk == vk) {
            wcscpy_s(out, (size_t)cap, kKeyNames[i].name);
            return;
        }
    }
}

/* 这个键的**功能文案**（表里来的，UTF-8）。
   ★ 两条不在动作表里的键在这里补：Caps / D / Alt / F 是**模式激活键**（"按住
   谁"这件事本身是语法，见 cap.h 的修饰模式表），不是"触发键 → 动作"；
   ` 是 hook.c 里特殊处理的诊断键（PROBE_KEY），它的文案借用 ACT_PROBE 那条 —— 
   文案仍然只有一份（在共享的键位表里）。 */
static const char* LabelOfVk(int vk) {
    if (vk == VK_CAPITAL) return "按住";
    if (vk == VK_LMENU)   return "增强";   /* 左 Alt 与 D 同为增强键 */
    if (vk == 'D')        return "增强";
    if (vk == 'F')        return "选区";
    if (vk == VK_OEM_3) {
        for (int i = 0; i < ACT_N; i++)
            if (kActions[i].act == ACT_PROBE) return kActions[i].label;
        return NULL;
    }
    for (int i = 0; i < ACT_N; i++)
        if (kActions[i].vk && kActions[i].vk == vk) return kActions[i].label;
    return NULL;
}

/* 圆角矩形（GDI+ 没有现成的：四段弧 + 闭合）。 */
static void AddRoundRect(Gdiplus::GraphicsPath* p, float x, float y,
                         float w, float h, float r) {
    float m = (w < h ? w : h) / 2.0f;
    if (r > m) r = m;
    if (r < 0.5f) r = 0.5f;
    float d = r * 2.0f;
    p->AddArc(x, y, d, d, 180.0f, 90.0f);
    p->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p->CloseFigure();
}

/* ---------- 文字：GDI+ 灰阶抗锯齿 ----------
   ★★ 为什么不上 ClearType：本窗口是**逐像素 alpha 的半透明浮层**，而 ClearType 是
   次像素渲染 —— 它对"文字下面是什么颜色"有假设，在带透明背景的位图上支持并不完美：
   会留下**彩色镶边**（看着像锯齿），深底亮字还会 blooming（"莫名其妙地粗"）。
   四种渲染并排实测过（GDI+ GridFit / GDI+ AntiAlias / GDI ClearType / GDI
   ANTIALIASED，同一块 DIB、同一字号）：ClearType 最粗且有彩边，ANTIALIASED 偏细软，
   **GDI+ 的 `AntiAliasGridFit` 在深底亮字上最锐利、且完全没有彩边**。
   ★ 去掉 ClearType 之后就只剩**字号**这一个变量了：灰阶抗锯齿在小字号下发虚。
   所以这里把能挤的空间全让给字号（键帽缝隙、标签留边都收到最小），并且
   **逐键帽实测**取"装得下的最大字号"，而不是拍一个比例 —— 以后改文案、改键位表，
   字号会自动跟着变，不需要回来调数字（`AllLabelsFit` + `PickFuncSize`）。 */
static const WCHAR* g_fontFace = L"Microsoft YaHei UI";

/* GDI+ 的 `MeasureString` 每侧会多算 1/6 em（这是它的既有行为，不是 bug）—— 直接拿
   它当判据会让字号白白小一号。这里按 em/3 把这份 padding 补回去。 */
static float TextWidth(Gdiplus::Graphics* g, const WCHAR* s, Gdiplus::Font* f, float em) {
    if (!s || !*s) return 0.0f;
    Gdiplus::RectF box(0.0f, 0.0f, 0.0f, 0.0f);
    g->MeasureString(s, -1, f, Gdiplus::PointF(0.0f, 0.0f), &box);
    return box.Width - em / 3.0f;
}

/* 这条文案按 `|` 切段后，每段都能装进这个键帽吗？（没写 `|` 的会走自动折行，
   用整条量是保守的。） */
static BOOL LabelLinesFit(Gdiplus::Graphics* g, Gdiplus::Font* f, const KeyDef* k,
                          float em, float u) {
    const char* lab = LabelOfVk(k->vk);
    if (!lab) return TRUE;
    float maxW = k->w * u - u * CAP_GAP_K - u * LABEL_PAD_K;
    WCHAR t[64];
    Utf8ToW(lab, t, 64);
    const WCHAR* seg = t;
    while (seg && *seg) {
        const WCHAR* bar = wcschr(seg, L'|');
        WCHAR one[64];
        if (bar) {
            size_t n = (size_t)(bar - seg);
            if (n > 63) n = 63;
            wcsncpy_s(one, 64, seg, n);
            seg = bar + 1;
        } else {
            wcscpy_s(one, 64, seg);
            seg = NULL;
        }
        if (TextWidth(g, one, f, em) > maxW) return FALSE;
    }
    return TRUE;
}

/* ---------- 功能名字号：**逐键帽**各定各的 ----------
   ★ 以前是"全局一个字号、由最长的文案卡死"—— F3 那种"打开|程序目录"（4 字一行）
   会把所有键都拖小，最后只能靠改文案换字号，不值。改成**按键算**：
   短文案的键用目标字号，长文案的键自动降一档，互不拖累，文案也就不必为字号让路。
   档位最多三四档，绘制时按字号取 Font（小缓存），不用每个键都建一个。 */
static float g_funcSize[LAYOUT_N];
static Gdiplus::Font* g_fontSlots[8] = { 0 };
static float g_fontSlotSize[8] = { 0 };

static Gdiplus::Font* FontFor(Gdiplus::FontFamily* fam, float size) {
    for (int i = 0; i < 8; i++)
        if (g_fontSlots[i] && g_fontSlotSize[i] == size) return g_fontSlots[i];
    for (int i = 0; i < 8; i++) {
        if (!g_fontSlots[i]) {
            g_fontSlotSize[i] = size;
            g_fontSlots[i] = new Gdiplus::Font(fam, size, Gdiplus::FontStyleRegular,
                                               Gdiplus::UnitPixel);
            return g_fontSlots[i];
        }
    }
    return NULL;   /* 八档都占满了（现实中到不了）：这颗键就不画功能名 */
}

static void FreeFontCache(void) {
    for (int i = 0; i < 8; i++) {
        delete g_fontSlots[i];
        g_fontSlots[i] = NULL;
    }
}

static void PickFuncSizes(Gdiplus::Graphics* g, Gdiplus::FontFamily* fam, float u) {
    for (int i = 0; i < LAYOUT_N; i++) {
        const KeyDef* k = &kLayout[i];
        if (!LabelOfVk(k->vk)) { g_funcSize[i] = 0.0f; continue; }
        float got = 9.0f;
        for (float h = g_fsFunc; h >= 9.0f; h -= 1.0f) {
            Gdiplus::Font f(fam, h, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            if (f.GetLastStatus() != Gdiplus::Ok) break;
            if (LabelLinesFit(g, &f, k, h, u)) { got = h; break; }
        }
        g_funcSize[i] = got;
    }
}

/* 画一行文本（垂直居中，按 al 水平对齐）。
   居中时把矩形**左右各放宽 10%**：GDI+ 的 `NoWrap` 在文字宽于矩形时是**直接裁掉**
   （"程序目录"→"程序目"）；放宽是对称的、不改变居中位置，只是不再被误判成溢出。 */
static void DrawLine(Gdiplus::Graphics* g, const WCHAR* s, Gdiplus::Font* f,
                     Gdiplus::Color c, float x, float y, float w, float h,
                     Gdiplus::StringAlignment al) {
    if (!s || !*s) return;
    Gdiplus::SolidBrush br(c);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(al);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::RectF rc = (al == Gdiplus::StringAlignmentNear)
                        ? Gdiplus::RectF(x, y, w, h)
                        : Gdiplus::RectF(x - w * 0.10f, y, w * 1.20f, h);
    g->DrawString(s, -1, f, rc, &fmt, &br);
}

/* 长文案折行：**最多两行**，中文逐字断、拉丁词不拆。
   断点优先级：文案里写死的 `|`（语义断点，见 keymap_table.h）> 按宽度自动找。 */
static int WrapLabel(Gdiplus::Graphics* g, const WCHAR* s, Gdiplus::Font* f, float em,
                     float maxW, WCHAR out[2][64]) {
    out[0][0] = out[1][0] = 0;
    if (!s || !*s) return 0;
    const WCHAR* bar = wcschr(s, L'|');
    if (bar) {
        size_t n = (size_t)(bar - s);
        if (n > 63) n = 63;
        wcsncpy_s(out[0], 64, s, n);
        wcscpy_s(out[1], 64, bar + 1);
        return out[1][0] ? 2 : 1;
    }
    int n = (int)wcslen(s);
    if (TextWidth(g, s, f, em) <= maxW || n < 3) {
        wcscpy_s(out[0], 64, s);
        return 1;
    }
    /* 从中间往两边找一个"两边都装得下"的断点；优先在中文/空格后断，别拆拉丁词。 */
    int best = -1;
    int mid = n / 2;
    for (int d = 0; d < n && best < 0; d++) {
        int cand[2] = { mid - d, mid + d };
        for (int k = 0; k < 2 && best < 0; k++) {
            int i = cand[k];
            if (i < 1 || i >= n) continue;
            /* 别把拉丁词拆开：断点两侧都是 ASCII 字母/数字就先跳过 */
            WCHAR a = s[i - 1], b = s[i];
            BOOL aWord = (a < 128 && (iswalnum(a) || a == '.'));
            BOOL bWord = (b < 128 && (iswalnum(b) || b == '.'));
            if (aWord && bWord) continue;
            WCHAR l[64], r[64];
            wcsncpy_s(l, 64, s, (size_t)i);
            wcscpy_s(r, 64, s + i);
            if (TextWidth(g, l, f, em) <= maxW && TextWidth(g, r, f, em) <= maxW) best = i;
        }
    }
    if (best < 0) {                       /* 找不到合适断点：硬折在中间 */
        best = mid;
        wcsncpy_s(out[0], 64, s, (size_t)best);
        wcscpy_s(out[1], 64, s + best);
        return 2;
    }
    wcsncpy_s(out[0], 64, s, (size_t)best);
    wcscpy_s(out[1], 64, s + best);
    return 2;
}

/* ---------- 所有文字（第二遍绘制） ----------
   顺序上必须晚于键帽那一遍：文字不会再被后画的相邻键帽盖住。 */
static void DrawAllText(Gdiplus::Graphics* g, Gdiplus::FontFamily* fam,
                        Gdiplus::Font* fN, Gdiplus::Font* fT, Gdiplus::Font* fFt,
                        float kbdX, float kbdY, float u, float capH) {
    DrawLine(g, APP_TITLE, fT, Gdiplus::Color(C_TITLE),
             g_pad, g_pad * 0.4f, (float)g_pxW - 2 * g_pad, g_title,
             Gdiplus::StringAlignmentNear);

    for (int i = 0; i < LAYOUT_N; i++) {
        const KeyDef* k = &kLayout[i];
        float x = kbdX + k->x * u;
        float y = kbdY + kRowY[k->row] * u;
        float w = k->w * u - u * CAP_GAP_K;

        const char* lab = LabelOfVk(k->vk);
        BOOL mapped = (lab != NULL);
        /* Caps / D / Alt 这三个修饰键用琥珀色：它们是"按住谁"的语法，
           与"按一下做什么"的动作在语义上不是一类，颜色上也就该分开。 */
        BOOL mod = (k->vk == VK_CAPITAL || k->vk == 'D' || k->vk == VK_LMENU);

        WCHAR name[32];
        KeyNameOf(k->vk, name, 32);
        /* 未映射的键也只画键名，且**位置与已映射键的键名一致**（都在键帽上部）——
           以前它居中，同一行里两种键名的基线高低不一，扫过去是跳的。 */
        DrawLine(g, name, fN,
                 Gdiplus::Color(mod ? C_MOD_NAME : (mapped ? C_ACT_NAME : C_DIM_TEXT)),
                 x, y + capH * 0.04f, w, capH * 0.34f, Gdiplus::StringAlignmentCenter);
        if (!mapped) continue;

        /* 已映射：**原键名留在键帽上**（上部），功能名在下面。
           键名在上、功能在下是刻意的：先认出"这是哪个键"，再看它做什么。
           功能名的字号是**这颗键自己的**（`PickFuncSizes` 按文案实测的），长文案
           的键自动用小一号，不拖累别的键。 */
        Gdiplus::Font* fF = FontFor(fam, g_funcSize[i]);
        if (!fF) continue;
        WCHAR labW[64], lines[2][64];
        Utf8ToW(lab, labW, 64);
        int nl = WrapLabel(g, labW, fF, g_funcSize[i], w - u * LABEL_PAD_K, lines);
        Gdiplus::Color cFunc(mod ? C_MOD_FUNC : C_ACT_FUNC);
        if (nl <= 1) {
            DrawLine(g, lines[0], fF, cFunc, x, y + capH * 0.36f, w, capH * 0.60f,
                     Gdiplus::StringAlignmentCenter);
        } else {
            DrawLine(g, lines[0], fF, cFunc, x, y + capH * 0.30f, w, capH * 0.34f,
                     Gdiplus::StringAlignmentCenter);
            DrawLine(g, lines[1], fF, cFunc, x, y + capH * 0.62f, w, capH * 0.34f,
                     Gdiplus::StringAlignmentCenter);
        }
    }

    /* 底部两栏：不是"某个键"的功能。键名用亮色、说明用灰 —— 与键帽同一套
       视觉语言（亮 = 要按的，灰 = 结果）。 */
    {
        float fy0 = kbdY + KBD_UNITS_H * u + g_gap;
        float colW = ((float)g_pxW - 2 * g_pad) / FOOTER_COLS;
        float rowH = g_foot / (float)FOOTER_ROWS;
        /* 键名区的宽度 = **本栏最长键名的实测宽度** + 一个间隔，而不是"半个栏宽"：
           以前键名贴最左、说明从栏中间开始，一行字被扯成断开的两半。按内容量宽之后，
           说明紧跟键名，两栏内部各自仍然是一列。 */
        float keyW[FOOTER_COLS];
        for (int c = 0; c < FOOTER_COLS; c++) {
            float wmax = 0.0f;
            for (int i = 0; i < FOOTER_N; i++) {
                if (i / FOOTER_ROWS != c) continue;
                WCHAR t[128];
                Utf8ToW(kFootKey[i], t, 128);
                float w = TextWidth(g, t, fFt, g_fsFoot);
                if (w > wmax) wmax = w;
            }
            keyW[c] = wmax + u * 0.30f;
        }
        for (int i = 0; i < FOOTER_N; i++) {
            int col = i / FOOTER_ROWS;      /* 先填满左边一列，再排右边 */
            int row = i % FOOTER_ROWS;
            float fx = g_pad + col * colW;
            float fy = fy0 + row * rowH;
            WCHAR kpart[128], text[128];
            Utf8ToW(kFootKey[i], kpart, 128);
            Utf8ToW(kFootDesc[i], text, 128);
            DrawLine(g, kpart, fFt, Gdiplus::Color(C_FOOT_KEY), fx, fy, keyW[col], rowH,
                     Gdiplus::StringAlignmentNear);
            DrawLine(g, text, fFt, Gdiplus::Color(C_FOOT_TEXT), fx + keyW[col], fy,
                     colW - keyW[col], rowH, Gdiplus::StringAlignmentNear);
        }
    }
}

/* ---------- 屏幕度量 ---------- */

static HMONITOR CursorMonitor(void) {
    POINT cur;
    if (!GetCursorPos(&cur)) cur.x = cur.y = 0;
    return MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
}

static void MonitorWork(HMONITOR mon, RECT* out) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) { *out = mi.rcWork; return; }
    out->left = 0; out->top = 0;
    out->right = GetSystemMetrics(SM_CXSCREEN);
    out->bottom = GetSystemMetrics(SM_CYSCREEN);
}

/* 该显示器的有效 DPI（96 = 100%）。动态取址：`GetDpiForMonitor` 在 shcore.dll
   里，只有 Win8.1+ 才有 —— 硬 import 会让本 exe 在更旧的系统上直接加载失败。 */
static int DpiOfMonitor(HMONITOR mon) {
    int dpi = 0;
    HMODULE m = LoadLibraryW(L"shcore.dll");
    if (m) {
        typedef HRESULT (WINAPI* PFN_GetDpi)(HMONITOR, int, UINT*, UINT*);
        PFN_GetDpi f = (PFN_GetDpi)(void*)GetProcAddress(m, "GetDpiForMonitor");
        if (f) {
            UINT dx = 0, dy = 0;
            if (SUCCEEDED(f(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx >= 96)
                dpi = (int)dx;
        }
        FreeLibrary(m);
    }
    return dpi ? dpi : 96;
}

/* ---------- 尺寸 ----------
   按"光标所在显示器的工作区"算一个 u（键位的单位长度，1u = 一个字母键的宽）。
   ★★ 所有长度都乘 dpi/96：本进程是 Per-Monitor V2，坐标与位图都是**物理**像素，
   而"多大算合适"是**逻辑**尺寸问题 —— 4K@200% 的物理工作区是 3840x2160，若把
   u 的上限写成 78 物理像素，图上每个键就只有 39 逻辑像素，比 1080p 屏上还小一半
   （这正是"高分屏上字又小又糊"的根因）。乘上缩放系数后，任何缩放率下看起来一样大，
   顺带让系统没有机会去缩放位图（那才是真的发虚）。
   基准值（96DPI 下）就是"1080p 屏上看着合适"的那一套：u=78、边距 24/26/40/16/84。 */
static void FitUnit(int* outW, int* outH) {
    HMONITOR mon = CursorMonitor();
    RECT work;
    MonitorWork(mon, &work);

    const float k = (float)DpiOfMonitor(mon) / 96.0f;
    const float margin = 24.0f * k;
    g_pad   = 26.0f * k;
    g_title = 40.0f * k;
    g_gap   = 16.0f * k;
    g_foot  = 24.0f * FOOTER_ROWS * k;   /* 底部两栏：基准行高 24，行数由条目数决定 */

    int u = (int)(50.0f * k + 0.5f);   /* ★ 整张图的"大小旋钮"：50 = 96DPI 下每个键位单位 50px */
    /* 放不下就缩（工作区特别小、或任务栏占掉一大截）。这一缩会让边距显得偏大，
       但只在"本来就装不下"的场合发生，优先保证整张图完整可见。 */
    int capW = (int)(((float)(work.right - work.left) - 2 * margin - 2 * g_pad) / KBD_UNITS_W);
    int capH = (int)(((float)(work.bottom - work.top) - 2 * margin - 2 * g_pad
                      - g_title - g_gap - g_foot) / KBD_UNITS_H);
    int cap = (capW < capH) ? capW : capH;
    if (u > cap) u = cap;
    if (u < 40) u = 40;      /* 兜底：再挤也不小于 40 物理像素，否则字看不清 */
    g_unit = u;      /* ★ 必须写回：开窗尺寸与绘制尺寸必须来自同一个 u，
                        否则就是"窗口按 78 开、内容按 64 画"，四周留一圈空白 */

    /* 字号跟着 u 走（都是 u 的定比例）：所以"整体放大/缩小"只需改上面那个 50。
       ★ 一律**取整**：非整数像素字号下，同一行里每个字的 hint 结果会不一致 —— 
       有的胖有的瘦，整体看着就是"字体怪怪的"。
       ★ 功能名的最终字号不由这里定：这个值只是个"上限目标"，每个键的真实字号由
       `PickFuncSizes` 按**各自的文案**实测往下找（长文案的键自动降一档，互不拖累）。 */
    g_fsName  = (float)(int)(u * 0.17f);
    g_fsFunc  = (float)(int)(u * 0.28f);
    g_fsTitle = (float)(int)(u * 0.23f);
    g_fsFoot  = (float)(int)(u * 0.22f);

    *outW = (int)(KBD_UNITS_W * u) + 2 * (int)g_pad;
    *outH = 2 * (int)g_pad + (int)g_title + (int)(KBD_UNITS_H * u)
            + (int)g_gap + (int)g_foot;
}

/* ---------- 渲染一帧 ----------
   分两遍：先铺所有键帽的底，再画所有文字 —— 文字不会被后画的相邻键帽盖住。 */
static BOOL Render(void) {
    if (!g_memDC || !g_bmp) return FALSE;

    const float u = (float)g_unit;
    const float capH = u - u * 0.11f;
    const float kbdX = g_pad;
    const float kbdY = g_pad + g_title;

    Gdiplus::Bitmap bmp(g_pxW, g_pxH, g_pxW * 4, PixelFormat32bppPARGB, (BYTE*)g_bits);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return FALSE;
    Gdiplus::Graphics g(&bmp);
    /* 底铺成**不透明**的背板色：ClearType 只能画在不透明的底上（见 C_PANEL）。
       旧版这里 Clear 成全透明 + 逐像素 alpha，代价就是字只能用灰阶抗锯齿。 */
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(Gdiplus::Color(C_PANEL));

    /* 字体**不跟随系统默认**：系统 UI 字体（`SPI_GETNONCLIENTMETRICS` 的 `lfMessageFont`）
       通常是 Segoe UI，中文会掉到回退字形（笔画细、像点阵），而这图上大半是中文。
       雅黑 UI 是 Win8+ 自带的 UI 版；取不到再退标准雅黑（字形差别很小）。 */
    {
        Gdiplus::FontFamily probe(g_fontFace);
        if (probe.GetLastStatus() != Gdiplus::Ok) g_fontFace = L"Microsoft YaHei";
    }
    Gdiplus::FontFamily fam(g_fontFace);
    /* ★ 功能名的字号**必须在这里定**：要逐个键帽量过才知道各自能放多大 */
    PickFuncSizes(&g, &fam, u);
    Gdiplus::Font fName(&fam, g_fsName, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fTitle(&fam, g_fsTitle, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fFoot(&fam, g_fsFoot, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    /* ---- 第一遍：背板 + 所有键帽的底 ---- */
    {
        /* 背板：半透明 + 一圈细边 */
        Gdiplus::GraphicsPath p;
        AddRoundRect(&p, 0.5f, 0.5f, (float)g_pxW - 1.0f, (float)g_pxH - 1.0f, PANEL_RADIUS);
        Gdiplus::SolidBrush bg(Gdiplus::Color(C_PANEL));
        Gdiplus::Pen pen(Gdiplus::Color(C_BORDER), 1.0f);
        g.FillPath(&bg, &p);
        g.DrawPath(&pen, &p);

        for (int i = 0; i < LAYOUT_N; i++) {
            const KeyDef* k = &kLayout[i];
            BOOL mapped = (LabelOfVk(k->vk) != NULL);
            BOOL mod = (k->vk == VK_CAPITAL || k->vk == 'D' || k->vk == VK_LMENU);
            Gdiplus::GraphicsPath path;
            AddRoundRect(&path, kbdX + k->x * u, kbdY + kRowY[k->row] * u,
                         k->w * u - u * CAP_GAP_K, capH, u * KEY_RADIUS_U);
            Gdiplus::SolidBrush fill(Gdiplus::Color(mod ? C_MOD_FILL : (mapped ? C_ACT_FILL : C_DIM_FILL)));
            Gdiplus::Pen stroke(Gdiplus::Color(mod ? C_MOD_LINE : (mapped ? C_ACT_LINE : C_DIM_LINE)), 1.0f);
            g.FillPath(&fill, &path);
            g.DrawPath(&stroke, &path);
        }
    }

    /* ---- 第二遍：所有文字 ---- */
    DrawAllText(&g, &fam, &fName, &fTitle, &fFoot, kbdX, kbdY, u, capH);
    g.Flush(Gdiplus::FlushIntentionSync);
    FreeFontCache();

    /* 位置 = 光标所在显示器工作区居中（物理像素 —— 本进程声明了 Per-Monitor V2） */
    RECT work;
    MonitorWork(CursorMonitor(), &work);
    int px = work.left + ((work.right - work.left) - g_pxW) / 2;
    int py = work.top + ((work.bottom - work.top) - g_pxH) / 2;
    if (px < work.left) px = work.left;
    if (py < work.top) py = work.top;
    if (g_hwnd) MoveWindow(g_hwnd, px, py, g_pxW, g_pxH, FALSE);

    /* 内容已画进双缓冲位图，交给 WM_PAINT 贴上去（不再走 UpdateLayeredWindow：
       那条路必须逐像素 alpha，而逐像素 alpha 画不出 ClearType）。 */
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}

static void MakeSurface(void) {
    if (g_bmp) return;
    HDC screen = GetDC(NULL);
    if (!screen) return;
    g_memDC = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_pxW;
    bi.bmiHeader.biHeight = -g_pxH;      /* 负 = 自上而下，省得算行序 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (g_bmp && g_memDC) SelectObject(g_memDC, g_bmp);
}

/* ---------- 显示 / 隐藏 ---------- */
static void DoShow(void) {
    KillTimer(g_hwnd, TID_GRACE);
    KillTimer(g_hwnd, TID_ARMED);
    if (g_holdMode) {
        /* 主程序在喂心跳，心跳就是寿命本身。这里先把它"喂"上一口，免得刚显示
           就被判成断供（SHOW 与 KA 谁先到是有先后的）。 */
        g_lastKa = GetTickCount64();
        SetTimer(g_hwnd, TID_KA, KA_POLL_MS, NULL);
    } else {
        /* 手动查看：没有人在喂，只能自己兜一个上限（也是唯一用到 TID_LIFE 的地方） */
        SetTimer(g_hwnd, TID_LIFE, LIFE_MS, NULL);
    }
    if (!g_visible) {
        Render();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_visible = TRUE;
    } else {
        Render();   /* 位置可能变了（光标换屏）：重画并重定位 */
    }
}

static void DoHide(void) {
    KillTimer(g_hwnd, TID_LIFE);
    KillTimer(g_hwnd, TID_ARMED);   /* 收到过状态了，启动防呆不必再留着 */
    if (g_visible) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_visible = FALSE;
    }
    /* 隐完再等 300ms 才退："刚松手又按下"时不至于来回重启进程。
       心跳此时已经停了，但 TID_KA 的判据要 g_visible 才生效 —— 这段缓冲
       不会被它打断，正好走完。（真有意外也顶多多活 800ms。） */
    SetTimer(g_hwnd, TID_GRACE, GRACE_MS, NULL);
}

static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgShow && msg == g_msgShow) {
        DoShow();
        return 0;
    }
    if (g_msgHide && msg == g_msgHide) {
        DoHide();
        return 0;
    }
    if (g_msgKa && msg == g_msgKa) {
        g_lastKa = GetTickCount64();   /* 只续命，不做别的 */
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TID_LIFE) { DoHide(); return 0; }
        if (wp == TID_GRACE) { DestroyWindow(hwnd); return 0; }
        if (wp == TID_ARMED) {
            /* --hold 下 1.5s 还没等到主程序给状态：它早就不需要这个窗口了
               （典型情形是"按下 30ms 就松手"，SHOW/HIDE 都发在窗口出生之前）。 */
            DestroyWindow(hwnd);
            return 0;
        }
        if (wp == TID_KA) {
            /* ★ 心跳断供 = 唯一需要的"收尾"判据：HIDE 丢了、主程序被强杀、
               主程序卡住，到这里都是同一件事。对端按 200ms 喂，这边给到
               800ms（4 个周期）—— 容忍消息队列瞬时拥塞，又远快于旧的 3 分钟。 */
            if (g_visible && GetTickCount64() - g_lastKa > HELP_KA_TIMEOUT_MS)
                DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_memDC && dc)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   g_memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* 内容全在双缓冲位图里，别让系统先擦一遍（会白闪） */
    case WM_LBUTTONDOWN:
        /* 点一下就关（手动模式下没有别的关闭手段：窗口不抢焦点，收不到 Esc） */
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;   /* 别让系统把点击解释成拖动标题栏 */
    default:
        break;
    }
    (void)wp; (void)lp;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- DPI 感知 ----------
   必须声明：不声明时窗口活在"系统按 96DPI 虚拟化"的坐标里，我们按物理像素画的
   位图会被系统再缩放一次（文字发虚、圆角糊）。这里与 settings.c 用的是同一条
   动态取址写法（不硬 import，免得在旧系统上直接加载失败）。 */
static void DpiAware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI* PFN_SetCtx)(HANDLE);
    PFN_SetCtx p = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (p) { p((HANDLE)(INT_PTR)-4); return; }    /* -4 = PER_MONITOR_AWARE_V2 */
    typedef BOOL (WINAPI* PFN_SetAware)(void);   /* SetProcessDPIAware 是无参的 */
    PFN_SetAware q = (PFN_SetAware)(void*)GetProcAddress(u, "SetProcessDPIAware");
    if (q) q();
}

/* SAL 批注与 SDK 里那份 wWinMain 声明保持一致：/analyze 会因为"一处有批注、
   一处没有"报 C28251。 */
int APIENTRY wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                      _In_ LPWSTR cmdLine, _In_ int show) {
    (void)hPrev; (void)show;
    g_holdMode = (cmdLine && wcsstr(cmdLine, L"--hold") != NULL);

    g_msgShow = RegisterWindowMessageW(HELP_MSG_SHOW);
    g_msgHide = RegisterWindowMessageW(HELP_MSG_HIDE);
    g_msgKa   = RegisterWindowMessageW(HELP_MSG_KA);

    /* 已经有一个在跑（上一轮还没退完）：把手动模式当成"再显示一次"，
       --hold 模式直接让位（主程序下 40ms 就会找到那个窗口并给它发状态）。 */
    HWND prev = FindWindowW(HELP_CLASS, NULL);
    if (prev) {
        if (!g_holdMode && g_msgShow) PostMessageW(prev, g_msgShow, 0, 0);
        return 0;
    }

    DpiAware();
    /* GdiplusStartup 是**进程级**的，且必须在任何 GDI+ 对象之前 —— 它也是本程序
       唯一的"重"初始化（毫秒级），没有 COM / TSF / 配置文件要读。 */
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &gsi, NULL) != Gdiplus::Ok) return 1;

    /* 三条"单击 Caps / 左 Shift / 右 Shift"的功能名来自配置文件：读一次覆盖默认值
       （读失败就保持默认）。与渲染无关，放在建窗之前顺手做掉。 */
    {
        int modes[3];
        LoadImeModes(modes);
        /* 索引与 kFootKey 尾部三条一一对应：单击 Caps / 左 Shift / 右 Shift */
        kFootDesc[4] = kImeDesc[modes[0]];
        kFootDesc[5] = kImeDesc[modes[1]];
        kFootDesc[6] = kImeDesc[modes[2]];
    }

    FitUnit(&g_pxW, &g_pxH);
    MakeSurface();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HelpProc;
    wc.hInstance = hInst;
    wc.lpszClassName = HELP_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassExW(&wc)) { Gdiplus::GdiplusShutdown(token); return 1; }

    /* WS_EX_NOACTIVATE：**绝不抢焦点**（本功能的核心要求）；TOOLWINDOW：不进任务栏
       与 Alt+Tab；TOPMOST：盖在别的窗口之上，但不激活它们。
       WS_EX_LAYERED 只用来做**整窗**透明（LWA_ALPHA）：内容照常走 GDI / WM_PAINT，
       文字因此能用 ClearType。旧版是逐像素 alpha + UpdateLayeredWindow，那条路
       画不出 ClearType —— 字发虚的根子在那。 */
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                             HELP_CLASS, APP_TITLE, WS_POPUP,
                             0, 0, g_pxW, g_pxH, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { Gdiplus::GdiplusShutdown(token); return 1; }
    SetLayeredWindowAttributes(g_hwnd, 0, HELP_LIFE_ALPHA, LWA_ALPHA);
    /* 圆角：内容现在是整块不透明背板，方角会露出来，用窗口区域把四角真正切掉 */
    HRGN rgn = CreateRoundRectRgn(0, 0, g_pxW + 1, g_pxH + 1,
                                  (int)PANEL_RADIUS * 2, (int)PANEL_RADIUS * 2);
    if (rgn) { SetWindowRgn(g_hwnd, rgn, FALSE); DeleteObject(rgn); }

    if (g_holdMode) {
        /* 先**只建窗不显示**，等主程序告诉我们当前该显还是该隐 —— 
           "按下 30ms 就松手"那种情形里，两条消息都发在窗口出生之前，
           只有等它来告诉（或在 1.5s 后放弃），才不会留下永久窗口。
           （不再需要自己盯主程序在不在：显示期间有心跳判据兜着。） */
        SetTimer(g_hwnd, TID_ARMED, ARMED_MS, NULL);
    } else {
        DoShow();   /* 手动查看：直接显示，点一下关，最长 3 分钟 */
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_bmp) DeleteObject(g_bmp);
    if (g_memDC) DeleteDC(g_memDC);
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
