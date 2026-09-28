/* ============================================================
   CapsEnhanceSettings.exe 的**内部**头：settings.c 与 sizepick.c 是同一个
   exe 的两个编译单元，这里放它们之间要共享的那几样。不对其它 exe 暴露
   （本体 / 帮助 / 浮层都不该 include 它）。

   为什么要有这么一个头：设置程序的主文件原本把"表单"和"尺寸选取窗"（一个
   自带窗口类、状态与两套自绘画法的小程序）装在一个编译单元里，两千七百行
   里两套主题互相稀释。拆开之后，两边只剩**一个方向的窄接口**：
     · 主文件给弹窗：窗口句柄 / DPI / 字体、字段表查询、读改写输入框的小工具；
     · 弹窗给主文件：这一对配没配「调整…」按钮、点它时开窗、注册窗口类。
   列在这里的每一项都是被迫共享的 —— 没在这个清单里的东西仍然各自 static
   在自己那个文件里（能不共享就不共享，这是拆分的全部收益所在）。
   ============================================================ */
#pragma once
#include "cap.h"

/* ---------- 控件 ID ----------
   两个编译单元共用：主文件按 id 建控件 / 落位，尺寸选取窗按 id 认按钮与
   它对应的那对输入框。放在共享头里，免得两边各抄一份数字（抄歪了就是
   "按钮没反应"这种查半天的错）。 */
/* ---- 控件 ID ---- */
#define IDC_SAVEONLY 101   /* 只保存不重启 */
#define IDC_STATUS  105
#define IDC_SAVE    102    /* 保存并重启 */
#define IDC_RELOAD  103
#define IDC_EXIT    104
#define IDC_TAB     120   /* 页签条（换页在 WndProc 的 WM_NOTIFY / TCN_SELCHANGE） */
#define IDC_HEADER  121   /* 顶部"窗框"托盘 */
#define IDC_FOOTER  122   /* 底部"窗框"托盘（hMenu 传 NULL 的窗口控件 ID 是 0，
                             会撞上"段头 id 为 0"这类查询 —— 必须给正式 ID） */
#define IDC_GROUP0  400   /* 常规页段头（每段一个 STATIC，见 kFields 的 group 列） */
#define GROUP_MAX   8     /* 段数上限；超出的段不带头（数据里目前 5 段，纯保底） */

#define IDC_RUN_ADMIN  200
#define IDC_LOG_KB     201
#define IDC_STEPS      202
#define IDC_OVER_CTRL  203
#define IDC_WMUL       204
#define IDC_INTERVAL   205
#define IDC_FASTMS     206
#define IDC_HMUL       207
#define IDC_SCI        208
#define IDC_IME_CAPS   209
#define IDC_IME_LSHF   210
#define IDC_IME_RSHF   211
#define IDC_DITTO_DB   212
#define IDC_CLIP_PICK  213   /* 按钮：显示当前字体 + 字号，点它弹系统字体对话框 */
#define IDC_CLIP_CLICK 214   /* 勾选框：单击条目直接上屏 */
#define IDC_DITTO_BROWSE 215 /* 按钮：选 Ditto 文件夹 */

#define IDC_DITTO_CONV   216 /* 按钮：DittoDir 相对/绝对路径互转（跨盘禁用） */
#define IDC_CLIP_W       217 /* 快速粘贴窗口宽（96 DPI 像素） */
#define IDC_CLIP_H       218 /* 快速粘贴窗口高（同上） */
#define IDC_CLIP_VMW     219 /* F3 预览窗格最大宽（96 DPI 像素） */
#define IDC_CLIP_VMH     220 /* F3 预览窗格最大高（同上） */
#define IDC_CLIP_VNW     221 /* F3 预览窗格最小宽（96 DPI 像素） */
#define IDC_CLIP_VNH     222 /* F3 预览窗格最小高（同上） */

/* 三对"窗口大小"各配一颗「调整…」按钮：**弹出一个照真实浮层画法画的示例
   窗口**，拖着它定尺寸，点中央「完成」把值填回这一对输入框（ini 仍由底部
   保存按钮写）。三颗按钮只有两套样式 —— F3 预览的最大与最小是同一个窗口。 */
#define IDC_CLIP_SIZE_BTN 223   /* 快速粘贴窗口（列表窗样式） */
#define IDC_VIEWMAX_BTN   224   /* F3 预览：最大（预览窗样式） */
#define IDC_VIEWMIN_BTN   225   /* F3 预览：最小（同上，同一套样式） */

/* 五份名单各一组：列表框 / 输入框 / 添加 / 删除 / 标题 / 选取窗口 */
#define IDC_LIST0  300
#define IDC_NAME0  301
#define IDC_ADD0   302
#define IDC_DEL0   303
#define IDC_LAB0   304
#define LIST_STRIDE 10

/* ---------- 字段表的一行（定义在 settings.c） ----------
   加一个配置项 = kFields 里加一行（另两处是 config.c 的 kCfg 表与 README 的
   配置表）。`cfg` 列指向 config.c 的配置项编号：Load / Save 按它直取直写，
   **没有分支**；值域也从那边取（InitFieldRanges），这里不写第二份。
   标签只写中文说明 —— ini 里的键名属于配置文件的内部拼法，界面不重复；
   数值单位（KB / 毫秒 …）单独一列，显示在输入框右侧。
   group 一列是**归属段**（Ditto 路径 / 快速粘贴显然是「剪贴板」这类）：
   每段第一行渲染一个半粗段头，同一段的行必须连排（段头按"换段"渲染）。
   isText 之所以要单独一位：文本行没有 min/max，也不该走"只能填数字"那条
   校验 —— 用 isBool 的三态去编码就会在校验分支里冒出 `if (isBool == 2)`
   这种暗号。 */
typedef struct {
    int          id;
    int          cfg;       /* 对应的 CFG_* 编号；-1 = 纯界面控件（按钮） */
    const WCHAR* label;
    const WCHAR* unit;      /* 数值单位，显示在输入框右侧；勾选框与文本框为 NULL */
    int          isBool;    /* 1 = 勾选框，0 = 输入框 */
    int          isText;    /* 1 = **文本**输入框（路径这类值）；与 isBool 互斥 */
    int          isBtn;     /* 1 = 按钮（label 就是按钮上的字） */
    int          min, max;  /* 值域**不在这里写第二份**：来自 config.c 的表 */
    const WCHAR* group;     /* 归属段名（段头文字就是它） */
} Field;

/* ---- 主窗口侧（settings.c 定义，弹窗要用）---- */
extern HWND  g_hwnd;        /* 设置窗口（弹窗以它为 owner，也靠它取输入框） */
extern int   g_dpi;         /* 当前窗口 DPI（96 = 100%） */
extern HFONT g_font;        /* 系统消息字体（按 DPI 取） */
extern HFONT g_fontBold;    /* 半粗体：段头 / 选中页签 / 弹窗上的强调文字 */

const Field* FieldById(int id);          /* 按控件 id 找字段行；没有返回 NULL */
int          PairSecondId(int id);       /* id 是某对的行首（宽）？返回行次（高）id，否则 0 */
void         SetStatus(const WCHAR* fmt, ...);
void         UpdateDirty(void);          /* 重新判定"有未保存改动"：与基线比对（装载期自动屏蔽） */
BOOL         ReadIntField(HWND edit, const Field* f, int* out);
void         SetIntField(HWND edit, int v);

/* ---- 弹窗侧（sizepick.c 定义，主文件要用）---- */
int  SizePickerBtnFor(int firstId);      /* 这一对尺寸配的「调整…」按钮 id（0 = 没配） */
void OpenSizePicker(int btnId);          /* 弹示例窗：拖完把值填回那一对输入框 */
BOOL SizePickerRegister(HINSTANCE hi);   /* 注册弹窗的窗口类（WinMain 里调，建窗之前） */
BOOL SizePickerIsOpen(void);             /* 弹窗开着吗（消息循环拿它拦 Esc / 让路 Tab） */
void SizePickerClose(BOOL save);         /* 收场：save = 把值填回输入框；FALSE = 放弃 */
void SizePickerShowButtons(BOOL show);   /* 三颗「调整…」随页显隐（表在弹窗那边） */
