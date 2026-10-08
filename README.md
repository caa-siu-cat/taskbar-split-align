# 开始靠左 · 图标居中（稳定版）

A [Windhawk](https://windhawk.net/) mod for Windows 11: **pins the Start button to the far left of the taskbar while the remaining icons stay centered.**

开始按钮固定到任务栏最左侧；其余图标由 Windows 原生布局重新居中。开始菜单仍按系统设置居中。

- **版本**: 1.0.0
- **Mod ID**: `taskbar-split-align`
- **加载目标**: 仅 `explorer.exe`（不挂钩开始菜单 / 搜索弹窗进程）
- **架构**: x86-64
- **许可**: GPL-3.0-only（派生自 m417z 的上游 mod，详见 [NOTICE](NOTICE)）

> 请保持 Windows 设置中任务栏对齐方式为 **“居中”**，否则本 mod 无意义。

---

## 效果

<img src="verification/final.png" alt="最终效果：开始按钮在最左侧，其余图标居中" width="820">

| 启用前 | 启用后 |
| --- | --- |
| <img src="verification/before.png" alt="启用前" width="400"> | <img src="verification/final.png" alt="启用后" width="400"> |

更多截图见 [`verification/`](verification/)：`installed.png`（安装界面）、`disabled.png`（停用后恢复原生）、`stress.png`（压力测试）。

## 实现方式

挂钩原生 `TaskbarCollapsibleLayout` 的 `Arrange` 布局过程，在排列时固定开始按钮，并把它占用的宽度从居中组里移除。

- 只识别 `AutomationId=StartButton`，不以“最左边的图标”猜测目标。
- 不使用定时器追赶动画，也不反复清零 / 叠加 `Translation`。
- 空间不足时给开始按钮保留位置，避免与应用图标重叠；原生任务栏仍会缩小图标或溢出。
- 不修改任务栏透明度。停用插件即可恢复原生布局。
- 请勿同时启用旧版或其他移动开始按钮的插件。

## 安装

1. 安装 [Windhawk](https://windhawk.net/)。
2. 新建 Mod → 粘贴 [`taskbar-split-align.wh.cpp`](taskbar-split-align.wh.cpp) 的内容 → 保存并启用。
3. 保持任务栏对齐方式为“居中”。

## 本地构建（可选）

需要本机已安装 Windhawk（脚本默认使用 1.7.3 的编译器路径）：

```powershell
.\build.ps1                 # 输出 build\local@taskbar-split-align_1.0.0.dll
.\build.ps1 -Version 1.0.1  # 指定版本号
```

无需额外工具链。若你的 Windhawk 版本不同，请调整 [`build.ps1`](build.ps1) 中的引擎 / 编译器路径。

## 验证

本机 Windows 11 25H2（26200.9457）、125% 缩放、单显示器：

- 完成 **56** 次测试窗口打开 / 关闭、**171+** 次压力布局采样，开始按钮 x 始终为 `0`。
- 最终静止图标组范围 `[848, 1200]`，中心 `1024`，与宽 2048 的任务栏中心一致。
- 停用 / 重新启用正常，Explorer 未重启；停用后恢复原生布局。
- 正常运行已关闭诊断与调试日志。

数据见 [`verification/results.json`](verification/results.json)，原始坐标轨迹见 [`verification/layout-trace.tsv`](verification/layout-trace.tsv)。复现脚本：[`capture-taskbar.ps1`](capture-taskbar.ps1) 与 [`verification/taskbar-test-window.cpp`](verification/taskbar-test-window.cpp)。

**未验证**：开始菜单的可视确认未完成；其他系统版本、多显示器、不同缩放均未实机验证。系统的图标增删动画会短暂移动应用图标（开始按钮不参与该动画）。

## 仓库结构

| 路径 | 说明 |
| --- | --- |
| `taskbar-split-align.wh.cpp` | Mod 源码（当前维护版本） |
| `build.ps1` | 本机构建脚本 |
| `capture-taskbar.ps1` | 任务栏截图脚本 |
| `verification/` | 几何记录、截图与测试结果 |
| `reference/` | 上游原版源码，仅作参考，未修改 |
| `backup/` | 旧版本源码与注册表备份 |

## 许可与归属

GPL-3.0-only。本仓库是 Michael Maltsev（[m417z](https://github.com/m417z)）的
[Start button always on the left 1.3.3](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-start-button-position.wh.cpp)
的修改版本，上游源码见 `reference/`。

本地改动摘要：仅固定开始按钮、移除开始菜单 / 搜索窗口的移动代码、改用原生 Arrange 布局固定、
独立修正开始按钮右键菜单锚点、嵌套排列状态恢复、可关闭的几何诊断。完整说明见 [NOTICE](NOTICE)。

GPL 许可全文：[LICENSE](LICENSE) · <https://www.gnu.org/licenses/gpl-3.0.txt>
