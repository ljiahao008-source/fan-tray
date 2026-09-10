# 机械革命监控（MechrevoMonitorTray）

机械革命（Mechrevo）笔记本的轻量硬件监控托盘工具：在任务栏托盘角落实时显示 **CPU 功耗**、**风扇转速**、**CPU 占用**、**CPU 温度**、**内存占用** 与 **网速上下行**，不弹窗、不占桌面，专注做好一件事。

## ✨ 特性

- **任务栏内嵌数据显示**：六项指标单行排列，数值在上、中文标签在下，深浅色任务栏自动适配
- **显示项可勾选**：设置对话框内自由开关每一项（全部取消时兜底显示功耗），配置写入 `config.json`
- **状态色预警**：正常 🟢 绿 / 偏高 🟠 橙 / 超高 🔴 红
  - 功耗阈值按 CPU 型号自动估算（HX/HK≈55W、H≈45W、P≈28W、U≈15W），≤TDP 绿、1~1.6×TDP 橙、>1.6×TDP 红
  - 风扇：≤3200 绿、3200~4800 橙、>4800 红；占用/内存：≤70% 绿、70~90% 橙、>90% 红；温度：≤75°C 绿、75~90°C 橙、>90°C 红
- **悬停统计卡片**：鼠标停在任一指标上即显示该指标的当前值 + 60 秒趋势图 + 最低/最高/平均
- **定宽布局**：每项按最坏值样本预测量定宽，数值位数进位/单位切换都不会引起窗口宽度变化（零抽动）
- **智能避让**：自动贴靠托盘角落，与 TrafficMonitor 等其他挂件互不重叠；explorer 崩溃后自动重嵌
- **资源占用极低**：纯原生 C++（无运行时），空闲 CPU ≈0%，内存工作集约 26MB、私有内存约 3.5MB
- **开机自启**：托盘右键即可开关；通过计划任务以最高权限自启（登录即提权、不弹 UAC，功耗数据开屏即有效）

## 📦 构建

需要 [Visual Studio 2022 Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe)（勾选"使用 C++ 的桌面开发"）。

```bat
build.cmd
```

产物：`MechrevoMonitorTray.exe`（约 250KB），运行需管理员权限（读取 CPU 功耗）。

## 🔧 技术栈

- **纯 C++17 / Win32**：无 .NET 运行时、无 WPF，界面用 GDI+ 手绘位图 + `UpdateLayeredWindow` 任务栏嵌入
- **CPU 功耗**：PawnIO 内核驱动读 RAPL 能量寄存器（MSR 差分），Intel 用 `IntelMSR.bin`、AMD（Zen，Family17h 体系）用 `AMDFamily17.bin`，启动时按 CPU 型号自动切换
- **CPU 温度**：PawnIO 读 AMD SMN 寄存器 `0x59800`（THM_TCON_CUR_TMP），公式对照 LibreHardwareMonitor
- **CPU 占用**：`GetSystemTimes` 差分；**内存**：`GlobalMemoryStatusEx`；**网速**：`GetIfTable` 字节计数差分（排除回环/隧道接口）
- **风扇转速**：机械革命私有 ACPI WMI（`PowerSwitchInterface`，每拍直读，同控制中心）
- 固件模块（`*.bin`）与 PawnIO 协议来自 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)（MPL 2.0，见 LICENSE 与 THIRD-PARTY-NOTICES.txt）

## 📁 目录

```
CppTray/
├── build.cmd            构建脚本
├── main.cpp             入口：单实例 / 采样线程 / 消息循环
├── monitor.cpp          功耗（MSR 差分）+ 温度（SMN）+ 占用/内存/网速 + 风扇 + 统计
├── widget.cpp           任务栏嵌入 + GDI+ 渲染 + 悬停卡片 + 避让
├── pawnio.cpp           PawnIO 驱动封装（固件加载 + MSR/SMN 读取）
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