# 剪贴板浮层设计说明（归档）

> 以下正文摘自移除前的 `docs/DEVELOPMENT.md`，保留原有的 Ditto / 剪贴板实现结论。

## 剪贴板浮层（`clip/clip.c` + `src/clip.c`）

一句话：`C + V` **按一下**就弹出一个不抢焦点的列表（Ditto 历史最近 100 条），挑一条粘回原来那个程序。窗口在**独立 exe** 里，分层窗口框架与 `help/help.c` 同源。

**为什么拆成独立 exe**：与帮助图同一条理由，但更重一层 —— 它要打开并查询别人的 SQLite 数据库、把一条记录的多个剪贴板格式解码后回写进系统剪贴板，这些都属于"可能慢、可能失败"的事，绝不能发生在承载钩子的主线程上。

### 与帮助窗口最关键的差别：寿命不跟和弦走

帮助是"按住 F1 才显示"，寿命 = 和弦（松开 Caps 时 `ResetEngine` 会 `HelpHide`）。剪贴板**不能这样**：松开 V 之后窗口必须还在，否则用户根本来不及挑条目。所以：

- 主程序侧**没有**"松开 Caps 就隐"这条路径，只有明确关闭途径；
- `ClipShow` 是 **toggle**（窗口已在 = 再按一次关掉它）；
- 由此多出一个必须堵的洞：`ClipIsOpen()` 直接返回 `s_want`，而钩子据此决定"要不要替浮层吞 Esc"。**送达超时时必须把 `s_want` 清掉** —— 否则窗口根本没起来，Esc 却被永久吃掉。

### 键盘：浮层收不到，全部由主程序的钩子代收

`WS_EX_NOACTIVATE` 意味着它一个键盘消息都收不到。所以 `hook.c` 在 `ClipIsOpen()` 期间拦截并转发：`Esc`、回车、`↑↓`、`PgUp/PgDn`、`Alt+数字`。拦的范围刻意很窄 —— **数字键只在 Alt 按着时才拦**，不带 Alt 的数字留给出路（也免得"浮层开着时打不出数字"）。

### 读 Ditto 的数据库（只读）

| 事实 | 出处 |
| --- | --- |
| 库是普通 SQLite：`Main`（`lID`/`mText`/`clipOrder`/`stickyClipOrder`/`bIsGroup`）+ `Data`（`lParentID`/`strClipBoardFormat`/`ooData`），正文在 `Data.ooData` 里**不压缩不加密** | Ditto `DatabaseUtilities.cpp` |
| 排序照 Ditto 自己那句：`stickyClipOrder DESC, clipOrder DESC`（`clipOrder` 才是它维护的顺序，不是 `lDate`） | Ditto `QPasteWnd.cpp` |
| 文本是 **UTF-16**（Ditto 的封装走 `sqlite3_column_text16`）→ 这里也必须 `sqlite3_column_text16` | Ditto `CppSQLite3.cpp` |
| 格式名到剪贴板格式号的映射是"标准名查表 + 其余 `RegisterClipboardFormat`" | Ditto `Misc.cpp` 的 `GetFormatID` |
| 默认 rollback journal（非 WAL）→ 只读打开安全，配 `busy_timeout` 扛"Ditto 正在写"的瞬间 | 同上 |

落地的几条取舍：

- **只取 `mText` 用作显示，正文留到选中时再查 `Data`** —— 100 条正文可能是 100 篇文章。
- "这是文本还是图片"靠 `group_concat(strClipBoardFormat)` 一次带出来，不为它再发 100 条查询。
- `sqlite3_open_v2` 只认 UTF-8 路径，所以宽路径先转一次；它是唯一带 flags 的打开接口，而**只读必须走它**（`sqlite3_open16` 没有 flags 参数，只能读写打开）。
- 路径的**相对基准由主程序展开**（它自己的 exe 目录）后用 `--db` 传给浮层 —— 基准只有那个进程知道，让子进程再解一次就是同一件事的两份实现。

### 粘贴：按键选择、注入标记与"先让开屏幕"

三条按顺序说明（合起来是一次 `DoPaste`）：

1. **先收起浮层，再抢前台**。写剪贴板成功之后立刻 `HideIfVisible()`，然后才 `SetForegroundWindow(目标)` + 轮询等它真的接管前台 —— 浮层留着会挡住用户看不到的粘贴落点，抢前台时还多一个置顶窗口当对手。**顺序不能反过来**：写剪贴板失败时要让用户看到浮层、能重试（那条分支直接 return，不隐藏）。退出计时（`DoHideClose`）留到最后一步，`HideIfVisible` 只负责"让开屏幕"。
2. **粘贴键按目标类型选**：控制台 / 终端系窗口（`ConsoleWindowClass`（本机用 AllocConsole 探针实测确认）、`CASCADIA_HOSTING_WINDOW_CLASS`、`mintty`、`PuTTY`）发 **`Shift+Insert`**，其余发 `Ctrl+V`。`Shift+Insert` 是这些窗口**共同**认的粘贴键，而 `Ctrl+V` 在 mintty / PuTTY 里是原样的控制字符、在 Vim 里是"可视块"—— 不换等于浮层对终端完全不可用。判据用**窗口类名**（WSL / cmd / PowerShell 全在同一个 conhost 类里，比按进程名判更全）。
3. **注入键打 `dwExtraInfo` 标记**（`CLIP_INJECT_MARKER = 'CLIP'`，定义在 `cap.h`，浮层与主程序共用一个常量）。`LLKHF_INJECTED` 只说明"有程序注入了键"、分不清是谁，于是钩子在那条放行之前多一条**显式**分支：认出自家的粘贴注入 -> 一律放行 + 记录模式记一行。它的价值是"不吞自家粘贴"这条不变量从此有代码保证（而不是依赖"注入的键恰好在最前面放行"这个位置巧合），并让记录日志能把"浮层注入的键"与"用户真实按键"分开 —— "点了条目没粘上"这类问题第一问就是"键到底发出去了没有"。

### 粘贴：不污染历史的关键是一个私有格式

粘贴必须把内容写进系统剪贴板，而 Ditto 正盯着剪贴板 —— 于是每次粘贴都会在历史里多出一条。可用的屏蔽手段实测只有一条走得通：

- **`"Clipboard Viewer Ignore"`**（Ditto 私有格式）：Ditto 在 `Clip.cpp` 与 `ClipboardViewer.cpp` 里**无条件**检查它（`IsClipboardFormatAvailable` 命中就 `return FALSE`），不需要用户在 Ditto 里开任何开关。
- `ExcludeClipboardContentFromMonitorProcessing` 需要用户先开 `m_enforceClipboardIgnoreFormats` 才生效，不能指望。
- `CopyAppExclude`（按活动窗口进程名排除）**对我们无效**：浮层是 `NOACTIVATE`，写剪贴板时活动窗口仍是目标应用 —— 排除它就等于把正常复制也一起废掉。
- Ditto 另有 CRC 去重（`Clip.cpp`），所以即便漏挂标记，重复也多半会被合并；但那取决于用户的"允许重复"设置，不能当保险。

另外两条：

- **原样回写所有格式**（图片 / `CF_HDROP` / RTF 都靠它），但要跳过 Ditto 的私有格式（`Ditto File Data`、`Ditto Remote CF_HDROP` …）和不是 `HGLOBAL` 的那几个标准格式（`CF_METAFILEPICT`、`CF_PALETTE`、`CF_ENHMETAFILE` …）。
- **抬 Alt 必须先塞一个中性键**（`0xFF`，AHK 的 `#MenuMaskKey` 那一招，与 `src/send.c` 的 `ReleaseMod` 同源）：用户是用 `C+V` 打开的，Alt 很可能仍物理按着，不先放掉 `Ctrl+V` 会被对端理解成 `Ctrl+Alt+V`。

### 色块与悬停提示：跟随 Ditto 的设置而不是自己的开关

**配置来源**：便携版 Ditto 的全部设置在 `<DittoDir>\Ditto.Settings`（UTF-16 LE 带 BOM 的 INI）。浮层启动后按需读一次（文件不存在就用默认值，**不建也不写**）：

| 键 | 默认（Ditto 同款） | 用途 |
| --- | --- | --- |
| `DrawCopiedColorCode` | `1` | 色值前画色块；`0` = 关 |
| `MaxToolTipCharacters` | `1000`（Ditto 源码再 +200 余量） | 悬停提示截断 |
| `MaxToolTipLines` | `30` | 提示最大行数 |
| `ToolTipTimeout` | `-1` = 5 秒；`0` = 关 | 提示自动消失 |

色值判据照抄 Ditto `QListCtrl.cpp` 的 `DrawCopiedColorCode`（`src/colorcode.c`，纯函数、有单测 `test/test_color.c`）：先 `Trim` 掉 `»` 后缀与空白、再剥结尾 `;`，然后依次试 W3C 命名色（141 个，表从 Ditto 源码脚本化提取）、`#RGB/#RGBA/#RRGGBB/#RRGGBBAA`、无前缀 6 位 hex、`0x` 前缀、`rgb()/rgba()`（支持百分比）、`hsl()/hsla()`。alpha < 255 时先画棋盘格再 alpha 混合，同 Ditto。

**悬停提示是自绘分层窗口**（照 Ditto 的 CToolTipEx 架构 —— 它也不是 tooltip 控件，
而是自绘 WS_POPUP）。圆角白底 + #CCC 边框 + 浮层行字体，`UpdateLayeredWindow`
逐像素 alpha、边缘 1.5px 羽化；显示 ~90ms 渐显、收起渐隐（`TID_TIPFADE` 步进，
每步重发一次 ULW，`SourceConstantAlpha` 当全局乘数，不用改位图）。窗口带
`WS_EX_TRANSPARENT`（鼠标穿透），压到光标也不打断悬停选中。
几个要点：

- **节奏两段式**：没显示时行变就重计 500ms（`TID_TIPSHOW`）—— 停稳才弹；
  已显示时行变 = 立即换内容并跟手移位（不收起、不重计）。弹出点按"当下"光标算。
- **时间行在最下面**：`Added: <lDate>` / `Last Used: <lastPasteDate>`（unix 秒、
  本地时区、不补零）。`lastPasteDate` 列不存在（很老的库）时整条 SELECT 退回旧
  查询、时间行不显示。
- **换行自己写**（`TipWrapInPlace`）：CJK 无空格按字断；ASCII 词内不断（超长词
  硬断）；空格断行时空格不进行尾。
- **GDI 不写 alpha**：文字和底色画完后再按圆角几何给整个 DIB 补 alpha，
  并按 a 预乘 RGB（`AC_SRC_ALPHA` 要求）。alpha 几何必须与 `RoundRect` 同一半径。
- **ULW 与 SLWA 互斥**：一旦调过 `SetLayeredWindowAttributes`，ULW 会失败 ——
  渐变只能走"重发 ULW + SourceConstantAlpha"这条路。
- 历史（系统 tooltip 控件路线，已弃）留档：无清单进程 = comctl32 v5，
  `TTM_ADDTOOLW` 传 v6 `sizeof(TOOLINFOW)` 静默失败（须 `TTTOOLINFOW_V1_SIZE`）；
  v5 的 `TTF_SUBCLASS` 在 NOACTIVATE 窗口上不触发；`TTM_POP` 对 TRACK 工具无效。
  弃用它就是因为观感"硬"：弹/收/切换全是瞬时的，调不出 Ditto 的"软"。


文本取法：条目没被截断（≤160 字符）直接用摘要；被截断的按需回库取全文（`TipTextOf`，与缩略图同一条"用到才付"的思路）。TTN_NEEDTEXT 里同步查库只针对当前悬停那一条，单条毫秒级。

### 焦点归还

用 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 记住"最后一个不是自己的窗口"（clippi 的做法）。为什么不能直接现取 `GetForegroundWindow`：用户在挑条目的过程中完全可能点过别处，而他真正要粘的是**打开浮层那一刻**在用的那个程序。同一个钩子顺带承担"点击外部 / 切到别的程序就自动关"（置顶期间与粘贴进行中停用 —— 粘贴自己会调 `SetForegroundWindow`，那条事件是**我们**引起的）。

### 测试边界

真机才能验的是"粘出来对不对"；能离线钉住的是接线与寿命语义（`test_chord` 场景 AI2）：`C+V` 接到 `ClipShow`、松开 Caps 之后仍开着（寿命 ≠ 和弦）、这期间 `Esc` 被吞并转发、**关掉后 `Esc` 恢复放行**。
