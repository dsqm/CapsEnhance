/* ============================================================
   CapsEnhanceSettings.exe 的内部头：`Field` 行定义与设置窗口共享声明。
   历史上尺寸选取窗曾是第二个编译单元、共用这个头；选取窗归档后，头文件保留，
   避免大改表单代码。不对其它 exe 暴露（本体 / 帮助都不该 include 它）。
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
#define IDC_HYPER      226   /* 勾选框：Hyper 透传（HyperPassthrough） */

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
   group 一列是**归属段**（例如「通用」「滚动」「输入法」）：
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

/* ---- 主窗口侧（settings.c 定义）---- */
extern HWND  g_hwnd;        /* 设置窗口 */
extern int   g_dpi;         /* 当前窗口 DPI（96 = 100%） */
extern HFONT g_font;        /* 系统消息字体（按 DPI 取） */
extern HFONT g_fontBold;    /* 半粗体：段头 / 选中页签 */

void SetStatus(const WCHAR* fmt, ...);
void UpdateDirty(void);          /* 重新判定"有未保存改动"：与基线比对（装载期自动屏蔽） */
BOOL ReadIntField(HWND edit, const Field* f, int* out);
void SetIntField(HWND edit, int v);
