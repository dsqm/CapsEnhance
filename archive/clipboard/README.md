# Caps+V 剪贴板浮层（归档）

这是 CapsEnhance 曾经的 `Caps+V` 剪贴板功能的完整源码快照，2026-10-27 从主程序移除。
功能本身不再随 CapsEnhance 编译或发布。

保留这份源码是因为实现里积累了不少试错结论：Ditto 数据库只读访问、剪贴板格式原样回写、
不污染历史、终端改用 `Shift+Insert`、图片预览与悬停提示等。设计说明见 [DESIGN.md](DESIGN.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/clip.c` | 主程序侧控制器：拉起浮层、握手、代收按键、转 Ditto 本体 |
| `clip/` | 浮层 exe 本体（`clip.c` / `clip_view.c` / `clip_thumb.c` / `clip.h`） |
| `src/colorcode.c` / `.h` | 色值识别（浮层专用，纯函数，有单测） |
| `settings/sizepick.c` | 设置程序里的窗口尺寸示例窗（依赖旧版 `settings/settings_int.h` 接口） |
| `test/test_color.c` | 色值识别单测 |
| `thirdparty/` | SQLite3 Multiple Ciphers amalgamation（浮层静态链接用） |

## 重新接回主程序需要恢复的东西

以下接线均已从当前代码移除：

- `build.bat`：sqlite 独立编译行、`colorcode.obj`、`clip/*.c` → `bin/CapsEnhanceClip.exe`
- `src/sources.txt`：`src\clip.c`
- `src/cap_core.h`：`ACT_CLIPLIST` / `ACTF_CLIPLIST`、`WM_CLIP_*`、`CLIPKF_*`
- `src/cap_overlay.h`：`CLIP_*` 常量与 `Clip*` 接口
- `src/cap_config.h` + `src/config.c`：`CFG_DITTO_DIR` / `CFG_CLIP_*` 配置项
- `src/keymap_table.h`：`Caps+V` 键位行
- `src/hook.c`：浮层打开期间的 Esc / 导航键 / F3 预览代收、单击 Caps 转 Ditto
- `src/main.c`：`Clip*` 消息分派、`CLIP_TIMER` / 预热定时器
- `settings/settings.c` + `settings/settings_int.h`：剪贴板字段、Ditto 路径浏览、字体对话框、`sizepick` 接线
- `test/build_analyze.bat`、`test/build_nav.bat`、`test/test_chord.c`、`test/test_config.c`：对应的分析与测试项

更完整的旧代码可从本仓库 git 历史中本次移除提交的父提交取出。
