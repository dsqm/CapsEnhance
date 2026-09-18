#ifndef CAPS_COLORCODE_H
#define CAPS_COLORCODE_H

#include <windows.h>

/* ============================================================
   剪贴板条目的"色值"识别 —— 照 Ditto 的 CQListCtrl::DrawCopiedColorCode
   的判据与顺序实现（oklch() 未实现，见 colorcode.c 的说明）。

   用法：拿到一条剪贴板文本 -> CcParseColor()。
     返回 TRUE  -> 它是个色值，*rgbOut 是颜色、*alphaOut 是 0..255 的 alpha，
                   调用方在文本左侧画一个色块（alpha<255 时先画棋盘格）。
     返回 FALSE -> 普通文本，照旧画。

   为什么要单独成模块：分支多（命名色 / #hex / 0x / rgb() / hsl() / 括号 /
   逗号 / 空格 / 6 位纯 hex），而且**无状态、不碰 Win32 API**，所以能像
   nav.c 的 ComputeSteps 那样进单测（test/test_color.c）。
   ============================================================ */
BOOL CcParseColor(const WCHAR* text, COLORREF* rgbOut, int* alphaOut);

#endif
