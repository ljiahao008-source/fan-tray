# 机械革命监控（MechrevoMonitorTray）

机械革命（Mechrevo）笔记本的轻量硬件监控托盘工具：在任务栏托盘角落实时显示 **CPU 功耗** 与 **风扇转速**，不弹窗、不占桌面，专注做好一件事。

## ✨ 特性

- **任务栏内嵌数据显示**：功耗（按状态着色）/ 风扇转速，数值在上、中文标签在下，深浅色任务栏自动适配
- **状态色预警**：正常 🟢 绿 / 偏高 🟠 橙 / 超高 🔴 红
  - 功耗阈值按 CPU 型号自动估算（HX/HK≈55W、H≈45W、P≈28W、U≈15W），≤TDP 绿、1~1.6×TDP 橙、>1.6×TDP 红
  - 风扇：≤3200 绿、3200~4800 橙、>4800 红
- **悬停统计卡片**：鼠标停在指标上即显示当前值 + 最低/最高/平均
- **智能避让**：自动贴靠托盘角落，与 TrafficMonitor 等其他挂件互不重叠；explorer 崩溃后自动重嵌
- **资源占用极低**：纯原生 C++（无运行时），空闲 CPU ≈0%，内存工作集约 20MB、私有内存约 3.5MB
- **开机自启**：托盘右键即可开关；通过计划任务以最高权限自启（登录即提权、不弹 UAC，功耗数据开屏即有效）

## 📦 构建

需要 [Visual Studio 2022 Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe)（勾选"使用 C++ 的桌面开发"）。

```bat
build.cmd
```

产物：`MechrevoMonitorTray.exe`（约 240KB），运行需管理员权限（读取 CPU 功耗）。

## 🔧 技术栈

- **纯 C++17 / Win32**：无 .NET 运行时、无 WPF，界面用 GDI+ 手绘位图 + `UpdateLayeredWindow` 任务栏嵌入
- **CPU 功耗**：PawnIO 内核驱动，Intel 用 `IntelMSR.bin`、AMD（Zen，Family17h 体系）用 `AMDFamily17.bin`，启动时按 CPU 型号自动切换
- **风扇转速**：机械革命私有 ACPI WMI（`PowerSwitchInterface`，每拍直读，同控制中心）
- 固件模块（`*.bin`）与 PawnIO 协议来自 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)（MPL 2.0，见 LICENSE 与 THIRD-PARTY-NOTICES.txt）

## 📁 目录

```
CppTray/
├── build.cmd            构建脚本
├── main.cpp             入口：单实例 / 采样线程 / 消息循环
├── monitor.cpp          功耗（MSR 差分）+ 风扇 + 统计 + 阈值
├── widget.cpp           任务栏嵌入 + GDI+ 渲染 + 悬停卡片 + 避让
├── pawnio.cpp           PawnIO 驱动封装（固件加载 + MSR 读取）
├── wmifan.cpp           ACPI WMI 风扇读取
├── config.cpp           config.json 持久化
├── autostart.cpp        计划任务开机自启
├── tray.cpp             托盘图标 + 右键菜单
├── settingsdlg.cpp      设置对话框
├── app.rc / app.manifest（requireAdministrator + PerMonitorV2 DPI）
└── IntelMSR.bin / AMDFamily17.bin   PawnIO 固件模块
```

## ⚠️ 历史说明

本项目最初为 C#/.NET + WPF 实现，于 3.2.9 后用纯 C++ 重写，资源占用从约 285MB / 63.7MB 降至约 20MB / 240KB。旧 C# 实现的运行程序已移除；相关源码仍保留在 git 历史（`git log`）中。