/* ================= cap_config：配置（CapsEnhance.ini） =================
   kCfg 描述符表的编号与通用读写接口 + 五份名单 + 进程名/路径查询。
   语义（默认值 / 值域 / 注释 / 解析）在 config.c 的表里，这里只有接口。
   本文件**只被 cap.h 包含**。 */
/* ---------- 配置模块（CapsEnhance.ini，与 exe 同目录） ---------- */
/* 名单编号。五份名单语义完全不同（改通道 / 整段不介入 / 只改滚动粒度 /
   拖动时按窗口类放过 / 拖动时按进程放过），所以是"五份"而不是"一份 + 几个开关"。
   编号放在这里是为了让 cap.h 就能表达"查哪份名单"（ConfigMatchList），
   而不必把 config.c 的枚举暴露出去。 */
#define CFG_LIST_NATIVE      0   /* [NativeWordJump]：不做文本探测，直接发 Ctrl+←/→ */
#define CFG_LIST_AUTODISABLE 1   /* [AutoDisable]：前台是它时整段不介入          */
#define CFG_LIST_NOSMOOTH    2   /* [NoSmoothScroll]：持续滚动只发整格           */
#define CFG_LIST_DRAGWIN     3   /* [DragWindowBlacklist]：按窗口类名，不许拖动  */
#define CFG_LIST_DRAGPROC    4   /* [DragProcessBlacklist]：按进程名，不许拖动    */
#define CFG_LIST_COUNT       5

/* 名单容量：设置程序要用它来定界面里的数组，所以放在这里而不是 config.c
   （同一份判据，别在两处各写一个 128 / 64）。 */
#define CFG_MAX_ENTRIES 128   /* 每份名单最多多少条 */
#define CFG_NAME_MAX    64    /* 单条进程名的最大字符数（含结尾的 0） */

/* 进程映像**全路径**的缓冲区（config.c 取路径 / folder.c 解析目标都用它）。
   刻意**不用 MAX_PATH**：Path 里有长目录、或程序装在长路径下时 260 会截断，
   而 QueryFullProcessImageNameW 支持更长的缓冲区 —— 拿到半截路径再去解析 PIDL
   只会指向一个不存在的文件。放这里是为了两处用同一个长度，不各写一个数。 */
#define PROC_PATH_MAX 1024

void ConfigLoad(void);                       /* 启动时读配置；缺失则写注释模板 */
void ConfigLoadFrom(const WCHAR* path);      /* 从指定路径加载（单测用） */
void ConfigUpgradeFile(const WCHAR* path);   /* 老配置补齐新增键（单测用） */
/* 升级键清单（只读）：单测据此**自动**生成"键齐全"的 fixture ——
   手写 fixture 时每加一个配置项都要记得同步，漏了就变成假失败。 */
int  ConfigUpgradeKeyCount(void);
const char* ConfigUpgradeKeyName(int i);     /* 越界返回 NULL */
BOOL ConfigMatchName(const WCHAR* exeName);  /* 进程名是否命中原生按词跳转名单 */
/* ---------- 配置项：编号 + 通用读写 ----------
   语义（键名 / 默认值 / 值域 / ini 注释 / 解析 / 写回）**只在 config.c 的
   kCfg 表里定义一次**，外面一律按 CFG_* 编号取用 —— 原先"一个配置项"要
   手工同步八处（常量 / 静态初值 / 解析分支 / ini 模板 / 补写块 / Get / Set /
   写回清单 / 设置界面的值域），漏一处**不会编译报错**，只会静默不一致
   （历史事故：默认值从 2 改成 3 时漏改了三处文案与两处范围）。
   加一个配置项 = config.c 的 kCfg 加一行 + settings.c 的 kFields 加一行
   （界面的标签 / 单位 / 分段属于呈现，留在设置程序那边）+ README 加一行。 */
enum {
    CFG_RUN_AS_ADMIN = 0,      /* [General] RunAsAdmin */
    CFG_LOG_MAX_KB,            /* [General] LogMaxKB */
    CFG_MAX_ARROW_STEPS,       /* [General] MaxArrowSteps */
    CFG_OVER_STEP_CTRL,        /* [General] OverStepMode */
    CFG_WHEEL_SCROLL_MUL,      /* [General] WheelScrollMul */
    CFG_AUTOSCROLL_INTERVAL,   /* [General] AutoScrollInterval */
    CFG_AUTOSCROLL_FAST_MS,    /* [General] AutoScrollFastMs */
    CFG_WHEEL_HSCROLL_MUL,     /* [General] WheelHScrollMul */
    CFG_ALLOW_SCINTILLA_READ,  /* [General] AllowScintillaRead */
    CFG_IME_CAPS_MODE,         /* [General] ImeCapsMode */
    CFG_IME_LSHIFT_MODE,       /* [General] ImeLShiftMode */
    CFG_IME_RSHIFT_MODE,       /* [General] ImeRShiftMode */
    CFG_DITTO_DIR,             /* [General] DittoDir（文本项） */
    CFG_CLIP_FONT,             /* [General] ClipFont（文本项） */
    CFG_CLIP_FONT_SIZE,        /* [General] ClipFontSize（磅） */
    CFG_CLIP_W,                /* [General] ClipW（窗口宽，96 DPI 下的像素） */
    CFG_CLIP_H,                /* [General] ClipH（窗口高，同上） */
    CFG_CLIP_CLICK_PASTE,      /* [General] ClipClickPaste */
    CFG_CLIP_VIEW_MAX_W,       /* [General] ClipViewMaxW（F3 预览窗格最大宽，96 DPI 下的像素） */
    CFG_CLIP_VIEW_MAX_H,       /* [General] ClipViewMaxH（F3 预览窗格最大高，同上） */
    CFG_COUNT
};

int  CfgInt(int id);                 /* 整数项当前值（布尔项为 0/1；越界返回 0） */
BOOL CfgBool(int id);                /* 布尔项当前值 */
const WCHAR* CfgStr(int id);         /* 文本项当前值（指向内部缓冲；越界返回空串） */
int  CfgLo(int id);                  /* 值域下限（设置程序校验用，与解析同源） */
int  CfgHi(int id);                  /* 值域上限 */
void CfgSetInt(int id, int v);       /* 改值：按表里的值域钳制（与解析同一份判据） */
void CfgSetBool(int id, BOOL v);
void CfgSetStr(int id, const WCHAR* v);   /* NULL = 清空；过长按缓冲截断，不 fastfail */

/* [General] DittoDir 的派生：<DittoDir>\Ditto.db 与 <DittoDir>\Ditto.exe。
   文本项的值用 CfgStr(CFG_DITTO_DIR) 取（返回内部缓冲，**不要写它**）。 */

/* [General] ClipFontSize 的取值范围由 CfgLo/CfgHi(CFG_CLIP_FONT_SIZE) 给出。 */
#define DITTO_DIR_MAX 320                   /* 路径缓冲区字符数（含结尾的 0）。
                                               比 MAX_PATH 宽一点：相对路径虽短，但拼出来的
                                               绝对路径可能落在较深的目录里。 */
/* [General] ClipFont / ClipFontSize：快速粘贴窗口的字体与字号（磅）。
   ★ 字号用**磅**而不是像素：系统字体对话框（ChooseFont）给的就是磅，两边
   不用换算 —— 存像素的话每次开对话框都要按 DPI 折回去，误差会累积。 */
#define CLIP_FONT_MAX  64                   /* 字体名缓冲（LF_FULLFACESIZE = 64） */
#define CLIP_FONT_SIZE_DEFAULT 10
#define CLIP_FONT_SIZE_MAX     72
/* 快速粘贴窗口的大小：单位是 **96 DPI 下的像素**（与窗口里其它尺寸同一套
   记法），实际像素 = 值 × 显示器 DPI/96 —— 存"当前显示器上看到的像素"会让
   同一份配置换个屏就变成另一个尺寸。设置界面里直接可改（剪贴板段）。
   默认 220x300 是按"与 Ditto 的快速粘贴窗口观感相近"取的（在 175% 缩放的
   屏幕上约为 385x525 物理像素）；嫌大嫌小都在设置里改。 */
#define CLIP_W_DEFAULT 220
#define CLIP_W_FALLBACK 220
#define CLIP_W_MIN     160
#define CLIP_W_MAX     1600
#define CLIP_H_DEFAULT 300
#define CLIP_H_FALLBACK 300
#define CLIP_H_MIN     140
#define CLIP_H_MAX     1600
/* [General] ClipViewMaxW / ClipViewMaxH：**F3 图片预览窗格**的最大宽/高
   （96 DPI 像素，实际 = 值 × 显示器 DPI/96，且仍不超过所在屏幕工作区）。
   默认取"工作区 70%"在 175% 缩放屏幕上的等效值（1024x530 逻辑像素），
   与旧版按百分比限制的行为在用户机器上完全一致。纯本程序配置，
   **不读 Ditto.Settings**。 */
#define CLIP_VIEW_MAX_W_DEFAULT 1024
#define CLIP_VIEW_MAX_W_MIN     160
#define CLIP_VIEW_MAX_W_MAX     8192
#define CLIP_VIEW_MAX_H_DEFAULT 530
#define CLIP_VIEW_MAX_H_MIN     140
#define CLIP_VIEW_MAX_H_MAX     8192
#define CLIP_CLICK_PASTE_DEFAULT 1  /* 单击条目直接上屏（默认开） */
/* 单键切换输入法：key 为 IME_KEY_*，返回 IME_MODE_*（0=不处理 1=中文 2=英文） */
int  ConfigGetImeMode(int key);
BOOL ConfigIsNativeWordJump(HWND focus);    /* 焦点窗口的进程是否命中 [NativeWordJump] */
BOOL ConfigIsAutoDisabled(HWND focus);      /* 前台窗口的进程是否命中 [AutoDisable]（命中 = 整段不介入） */
BOOL ConfigIsDragBlacklisted(HWND hwnd);    /* 是否命中两份拖动黑名单之一（[DragWindowBlacklist] 按类名 / [DragProcessBlacklist] 按进程名） */
BOOL ConfigIsNoSmoothScroll(HWND focus);    /* 前台窗口的进程是否命中 [NoSmoothScroll]（命中 = 持续滚动只发整格） */
BOOL ConfigMatchList(int list, const WCHAR* exeName); /* 进程名是否命中指定名单（CFG_LIST_*） */
BOOL ConfigGetProcessName(HWND focus, WCHAR* out, size_t cap); /* 焦点窗口的进程 exe 名（不含目录） */
BOOL ConfigGetProcessPath(HWND focus, WCHAR* out, size_t cap); /* 焦点窗口的进程映像**全路径** */
BOOL ConfigGetProcessPathByPid(DWORD pid, WCHAR* out, size_t cap); /* 同上，按 PID 取（UWP 的真实进程不是前台窗口那个） */
BOOL IsRunElevated(void);                   /* 真实运行权限（查进程令牌） */

/* ---- 设置程序（settings/settings.c）用：改值 + 增量写回 ----
   只给设置程序用：它改的是"某个配置项"，不是整份文本。钳制与解析同源
   （config.c 的 Clamp*），写回保留注释与未知段（规则见 ConfigSaveFile）。
   新增配置项时这里也要加一个 Set —— 与 Get 成对，漏了就是"界面改不动它"。 */

void ConfigSetImeMode(int key, int v);      /* key = IME_KEY_*，v = IME_MODE_*（越界按键忽略） */
void        ConfigSetList(int list, const WCHAR* const* names, int n); /* 整份替换 */
int         ConfigListCount(int list);
const WCHAR* ConfigListAt(int list, int i);
BOOL ConfigSaveFile(const WCHAR* path);      /* 写回 ini；失败返回 FALSE（文件不落盘） */
