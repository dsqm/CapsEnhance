#ifndef INFO_SHARED_H
#define INFO_SHARED_H

/* ================= 窗口信息弹窗（C + F12）的共享常量 =================
   窗口本体在**独立 exe**（info/info.c → bin/CapsEnhanceInfo.exe）里，主程序侧
   只负责"拉起 / 前置 / 告诉它看哪个窗口"（见 src/info.c）。三处消费者各自
   include 这同一份定义，谁都不抄第二份：

     · src/info.c            拉起与前置、窗口尺寸回存
     · info/info.c           弹窗本体
     · src/config.c          配置项的默认值与值域（InfoFont / InfoFontSize / InfoW / InfoH）
     · settings/settings.c   字体对话框的字号范围（与配置项同源）

   为什么独立 exe、而不是在主程序里开窗口：主程序的主线程是**键盘钩子宿主**。
   本功能要 EnumChildWindows 一棵可能几百个节点的树、要跨进程读标题、要
   OpenProcess + 令牌查询，任何一步遇到无响应或受保护的进程都可能阻塞到超时；
   钩子回调里阻塞会触发系统的 LowLevelHooksTimeout 并被静默摘钩（整个程序失灵）。
   设置程序与帮助窗口出于同一条理由也是独立 exe（见 build.bat 的注释）。 */

/* 窗口类名 / exe 名：跨进程握手靠 FindWindowW(INFO_CLASS) —— 注册消息
   （>= 0xC000）是 Windows 给"两个自有进程之间说私话"准备的正规通道，
   两边用同名注册即可拿到同一个消息号，不需要额外的 IPC 机制。 */
#define INFO_CLASS     L"CapsEnhanceInfo"
#define INFO_EXE       L"CapsEnhanceInfo.exe"
#define INFO_MSG_OPEN  L"CapsEnhanceInfoOpen"  /* wParam = 目标窗口 HWND（0 = 保留上次的） */
#define INFO_MSG_SIZE  L"CapsEnhanceInfoSize"  /* wParam = 宽(DIP)，lParam = 高(DIP)：
                                                  弹窗关窗时把用户拖出来的尺寸回存进
                                                  ini —— 写文件由主程序做（它已经有
                                                  config.c 的增量写回，弹窗自己重写
                                                  一遍就等于多一份容易写坏 ini 的代码） */

/* ini 键名。**同一份文件里并排放着两种形态不是笔误**：config.c 的 kCfg 表用
   char（它整表都是 char，还要按 UTF-8 写进文件），弹窗直接读 ini 则需要 WCHAR。
   两行挨着放，改一个的时候另一个就在眼皮底下。段一律是 [General]：增量写回
   （ConfigSaveFile）只认这一段里的键，另开一段会让新键被写回 [General]、
   而原文那段留着旧值 —— 同一项出现两处，谁生效取决于解析顺序。 */
#define INFO_INI_FONT      "InfoFont"
#define INFO_INI_FONTSIZE  "InfoFontSize"
#define INFO_INI_W         "InfoW"
#define INFO_INI_H         "InfoH"

/* 字号：磅（与系统字体对话框和 ini 里的 InfoFontSize 同单位）。
   下限 6 是"再小就看不清"，上限 72 是"再大一行放不下几个字"。 */
#define INFO_FONT_SIZE_DEFAULT 12
#define INFO_FONT_SIZE_MIN     6
#define INFO_FONT_SIZE_MAX     72

/* 窗口尺寸：96 DPI 下的逻辑像素（与其余配置项的"与 DPI 无关"约定一致）。
   实际像素 = 值 × 目标屏 DPI / 96。高度比宽度大得多，是因为字段是竖排的
   一长列（窗口层十余项 + 进程层十余项 + 子窗口树），默认值要能一眼看到进程层
   而不必先滚动。
   ★ 高度定 700 而不是"越大越好"：175% 缩放的笔记本上，1080p 屏扣掉任务栏后
   只有 706 DIP 可用 —— 更大的默认值在那里会被"钳进工作区"砍掉，等于没设。 */
#define INFO_W_DEFAULT 620
#define INFO_H_DEFAULT 700
#define INFO_W_MIN     320
#define INFO_W_MAX     2000
#define INFO_H_MIN     200
#define INFO_H_MAX     2000

#endif /* INFO_SHARED_H */
