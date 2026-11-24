#ifndef PLAIN_TEXT_H
#define PLAIN_TEXT_H

#include <windows.h>

/* ================= 纯文本粘贴的纯逻辑（src/plain_text.c） =================
   为什么单独一个 TU：判定"有没有内容"、把文件拼成文本、认不认这个窗口类
   属终端 —— 这三件事全是**纯函数**，不碰剪贴板、不发键，可以被单测直接验证
   （与 copy_text.c 同一个理由：把决策与"抢剪贴板 + 注入按键"分开，测试才
   不需要真的去改这台机器的剪贴板、也不需要往别的程序里打字）。

   ★ 它们服务 Caps+V（见 src/plainpaste.c）：三家参考实现（Ditto / clippi /
     QuickClipboard）都证明"纯文本粘贴"的正确形态是「内容收成纯文本 + 发粘贴键」，
     而其中真正需要小心的是**边界判定**而不是发送本身。 */

/* 文本是否等于"没有内容"（空串，或全是空白）。
   空白按 Unicode 口径：ASCII 空格 / 制表 / 换行 / 垂直制表 / 换页，外加
   全角空格 U+3000 与不换行空格 U+00A0 —— 中文输入法下按空格很可能就是
   U+3000，把它当内容粘出去等于粘了一片看不见的东西。
   口径与 clippi 的 `paste_plain.rs` 一致（它用 Rust 的 trim()，同样是 Unicode 空白）。 */
BOOL PlainIsBlank(const WCHAR* s, int len);

/* 把若干路径拼成一段文本：**每行一个完整路径，行间用 LF，末行不带换行**。
   ★ 行间用 LF 不是笔误：与 copy_text.c 同一套内部约定 —— 内部一律 LF，
     只在写进剪贴板的那一刻统一转 CRLF（`plainpaste.c` 的 PlainWriteText）。
     两边各用一套换行、各自展开一次，就会出现 `\r\r\n`（粘出去多一个空行）。
   为什么不带尾部换行：这段文本经常被粘进终端 / 命令行，末尾一个回车就变成
   "顺手执行了"。不加引号同理 —— 引号会原样出现在文本框里（参考实现
   Ditto 的 CreateCF_HDROPBufferAsString 与 clippi 的规则 2 都是这个形态）。
   out 的容量 cap 以 WCHAR 计（含结尾 0）；返回写入的长度（不含结尾 0），
   容量不够时返回 -1 并把 out 置空串（调用方按"没拼出来"处理）。 */
int PlainJoinPaths(const WCHAR* const* paths, int n, WCHAR* out, int cap);

/* 这个窗口类名是不是"Ctrl+V 不管用"的控制台 / 终端系。
   ★ 这不是"多支持一种粘贴方式"，是补一个真实缺口：mintty（Git Bash）、PuTTY、
     Vim 里 Ctrl+V 要么是原样的控制字符、要么是"可视块"。Shift+Insert 是它们
     **共同**认的粘贴键（conhost / Windows Terminal / mintty / PuTTY 都支持）。
   判据用窗口**类名**而不是进程名：conhost 的类名是固定的（本机实测
   ConsoleWindowClass），而 cmd / PowerShell / WSL 全跑在同一个类里。
   名单与归档的剪贴板历史（archive/clipboard/clip/clip.c 的 TargetIsConsole）同源。 */
BOOL PlainIsConsoleClass(const WCHAR* cls);

#endif /* PLAIN_TEXT_H */
