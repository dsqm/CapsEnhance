#ifndef INFO_FIELDS_H
#define INFO_FIELDS_H

#include <string.h>   /* strcmp（InfoFieldByKey 用） */

/* ================= 窗口信息弹窗的字段注册表（info 与 settings 共用） =================
   「常用」视图按用户配置的有序字段列表渲染，「全部」视图按同一张表逐项输出，
   "设置里管理常用字段"也要同一份词汇表 —— 三处都 include 这份。它只被
   info/info.c 与 settings/settings.c 包含（src/info.c、config.c 不需要，
   别把未用的表拖进去惹 /W4 的未用告警）。

   · key     ：存进 ini 的稳定短标识（ASCII，改它等于让旧配置失效，别乱改）
   · section ：0=窗口层 1=进程层（设置里按层分组展示、弹窗按层排版都用它）
   · name    ：显示名。**两个视图共用这一个名字** —— 同一项在两处叫法不同
               （「常用」叫"窗口标题"、「全部」叫"标题"）会让人怀疑是不是两回事。

   顺序即枚举顺序；新增字段 = 这里加一行 + Collect 里用该 id 发一条值
   （BufField(&b, IF_XXX, ...)：名字从这里取，值顺带按 id 存给「常用」视图）。 */
typedef struct {
    const char*  key;
    int          section;
    const WCHAR* name;
} InfoFieldDef;

enum {
    IF_TITLE = 0, IF_TITLELEN, IF_CLASS, IF_HANDLE, IF_STATE,
    IF_RECT, IF_CLIENT, IF_MONITOR, IF_DPI,
    IF_PARENT, IF_OWNER, IF_ROOT, IF_ROOTOWNER, IF_TID,
    IF_STYLE, IF_STYLESYM, IF_EXSTYLE, IF_EXSTYLESYM,
    IF_HOSTNOTE,
    IF_PROCNAME, IF_PID, IF_IMAGE, IF_VER, IF_PRODUCT, IF_COMPANY,
    IF_ARCH, IF_CMDLINE, IF_CWD, IF_START, IF_PPROC,
    IF_ACCOUNT, IF_INTEGRITY, IF_ELEVATION, IF_SESSION, IF_HANDLES, IF_WORKSET,
    IF_COUNT
};

static const InfoFieldDef kInfoFields[IF_COUNT] = {
    { "title",      0, L"窗口标题" },
    { "titlelen",   0, L"标题长度" },
    { "class",      0, L"类名" },
    { "handle",     0, L"句柄" },
    { "state",      0, L"状态" },
    { "rect",       0, L"窗口位置" },
    { "client",     0, L"客户区" },
    { "monitor",    0, L"显示器" },
    { "dpi",        0, L"窗口 DPI" },
    { "parent",     0, L"父窗口句柄" },
    { "owner",      0, L"所有者窗口句柄" },
    { "root",       0, L"根窗口句柄" },
    { "rootowner",  0, L"根所有者窗口句柄" },
    { "tid",        0, L"线程 ID" },
    { "style",      0, L"样式" },
    { "stylesym",   0, L"样式符号" },
    { "exstyle",    0, L"扩展样式" },
    { "exstylesym", 0, L"扩展样式符号" },
    { "hostnote",   0, L"窗口宿主" },
    { "procname",   1, L"进程名称" },
    { "pid",        1, L"PID" },
    { "image",      1, L"程序路径" },
    { "ver",        1, L"文件版本" },
    { "product",    1, L"产品名" },
    { "company",    1, L"公司" },
    { "arch",       1, L"架构" },
    { "cmdline",    1, L"命令行" },
    { "cwd",        1, L"当前目录" },
    { "start",      1, L"启动时间" },
    { "pproc",      1, L"父进程" },
    { "account",    1, L"账户" },
    { "integrity",  1, L"完整性级别" },
    { "elevation",  1, L"运行权限" },
    { "session",    1, L"会话" },
    { "handles",    1, L"句柄数" },
    { "workset",    1, L"工作集" },
};

/* 默认「常用」：默认显示的那 10 项，顺序即显示顺序（逗号分隔的 key）。 */
#define INFO_COMMON_DEFAULT \
    "title,procname,rect,image,pid,class,handle,parent,root,rootowner"

/* 按 key 找 id，找不到返回 -1。inline：只被一个消费者用到时也不触发未用告警。 */
static inline int InfoFieldByKey(const char* key) {
    for (int i = 0; i < IF_COUNT; i++)
        if (strcmp(kInfoFields[i].key, key) == 0) return i;
    return -1;
}

#endif /* INFO_FIELDS_H */
