# 第三方代码声明

CapsEnhance 本身以 **GPL-3.0-or-later** 发布（见根目录 [LICENSE](LICENSE)）。本文件列出项目引入的第三方**代码**来源及其许可证。下列许可证均与 GPL-3.0 兼容，无许可证冲突。

> 说明：IMM32 `WM_IME_CONTROL` 命令常量、IME 状态模型、ANSI-104 键位几何等属于公开 API 协议与事实标准，不构成受版权保护的代码表达，故不在本文件列为代码来源。

---

## 1. AltSnap —— GPL-3.0-or-later

- 上游：<https://github.com/RamonUnch/AltSnap>
- 引入位置：`src/drag.c`、`src/config.c`
- 引入内容：`Caps+左键拖动窗口`的交互与实现思路，以及 `[DragWindowBlacklist]` / `[DragProcessBlacklist]` 的默认名单。

```
AltSnap: Copyright (C) 2015 Stefan Sundin
         Modified By Raymond Gillibert in 2021
         https://github.com/RamonUnch/AltSnap
```

AltSnap 以 GPL-3.0-or-later 授权，与本项目许可证相同，全文见根目录 [LICENSE](LICENSE)。

---

## 2. Microsoft PowerToys（AlwaysOnTop 模块）—— MIT

- 上游：<https://github.com/microsoft/PowerToys>
- 引入位置：`src/pin.cpp`（整模块移植：AlwaysOnTop / WindowBorder / FrameDrawer / ScalingUtils / WindowCornersUtil / VirtualDesktopUtils / Sound）
- 版权声明与 MIT 全文同时保留在 `src/pin.cpp` 文件头。

```
The MIT License

Copyright (c) Microsoft Corporation. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

MIT 为宽松许可证，单向兼容并入 GPL-3.0。

---

## 3. IMEIndicator —— GPL-3.0-or-later（同作者）

- 上游：<https://github.com/dsqm/IMEIndicator>
- 引入位置：`help/help.c`
- 引入内容：按键帮助浮层的分层窗口实现思路（`WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` + 32bpp DIB Section + `UpdateLayeredWindow(ULW_ALPHA)`）。

```
IMEIndicator: Copyright (C) 2026  qm
```

与本项目同作者、同为 GPL-3.0-or-later，全文见根目录 [LICENSE](LICENSE)。

---

## 4. Ditto —— GPL-3.0（已归档）

- 上游：<https://github.com/sabrogden/Ditto>
- 引入位置：`archive/clipboard/`（已不再编译或随产品发布，源码快照随仓库分发）
- 引入内容：浮层的窗口/文字画法、默认主题配色、Ditto 数据库只读访问定义。
- 作者：Scott Brogden。

Ditto 以 GPL-3.0 授权，与本项目 GPL-3.0-or-later 兼容，全文见根目录 [LICENSE](LICENSE)。

---

## 5. SQLite3 Multiple Ciphers（sqlite3mc）v2.3.5 —— MIT（已归档）

- 上游：<https://github.com/utelle/SQLite3MultipleCiphers>
- 引入位置：`archive/clipboard/thirdparty/sqlite3mc_amalgamation.c` / `.h`（vendored amalgamation，已不再编译）
- 版权：`(c) 2019-2026 Ulrich Telle`，主体以 MIT 授权。

完整许可证已逐字内嵌在 amalgamation 源文件头部并原样保留。其内嵌子组件包括：

| 子组件 | 版权持有人 | 许可证 |
| --- | --- | --- |
| SQLite 核心 | 作者放弃版权 | Public domain |
| sqlite3mc 主体 / 各 codec | Ulrich Telle | MIT |
| AEGIS / AES 硬件检测 | Frank Denis | MIT |
| MD5 | Alexander Peslyak | Public domain（附 BSD 式兜底） |
| SHA-224/256/384/512 | Olivier Gay | BSD-3-Clause |
| miniz | 多作者 | Public domain |

各子组件的完整声明见对应源文件头部。