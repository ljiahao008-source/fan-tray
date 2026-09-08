# 机械革命监控（MechrevoMonitorTray）

机械革命（Mechrevo）笔记本的轻量硬件监控托盘工具：在任务栏托盘角落实时显示 **CPU 功耗** 与 **风扇转速**，不弹窗、不占桌面，专注做好一件事。

![version](https://img.shields.io/badge/version-3.2.8-blue)

## ✨ 特性

- **任务栏内嵌数据显示**：功耗（按状态着色）/ 风扇转速，数值在上、中文标签在下，深浅色任务栏自动适配
- **状态色预警**：正常 🟢 绿 / 偏高 🟠 橙 / 超高 🔴 红
  - 功耗阈值按 CPU 型号自动估算（HX/HK≈55W、H≈45W、P≈28W、U≈15W），≤TDP 绿、1~1.6×TDP 橙、>1.6×TDP 红
  - 风扇：≤3200 绿、3200~4800 橙、>4800 红
- **悬停统计卡片**：鼠标停在指标上即显示当前值 + 最低/最高/平均
- **智能避让**：自动贴靠托盘角落，与 TrafficMonitor 等其他挂件互不重叠；explorer 崩溃后自动重嵌
- **资源占用极低**：后台线程采样（采样不卡 UI）、空闲零渲染失效，CPU 占用 <0.5%（单核）
- **开机自启**：托盘右键即可开关；通过计划任务以最高权限自启（登录即提权、不弹 UAC，功耗数据开屏即有效）

## 📦 下载安装

前往 [Releases](https://github.com/ljiahao008-source/fan-tray/releases) 下载 `MechrevoMonitorTray-Setup-x.y.z.exe`：

1. 双击安装（内置中文安装向导，可选桌面快捷方式 / 开机自启）
2. 启动后自动嵌入任务栏，右键托盘图标可打开设置、重置统计或退出
3. 卸载请用系统的「应用和功能」或开始菜单里的卸载快捷方式

> 读取 CPU 功耗需要管理员权限，启动时会弹 UAC 确认框。

## 🔧 技术栈

- .NET 8 / WPF，基于 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) 精简（MPL 2.0，见 LICENSE 与 THIRD-PARTY-NOTICES.txt）
- 风扇转速：机械革命私有 ACPI WMI（PowerSwitchInterface，LHM 原生不支持），与控制中心一致每拍直读
- 安装包：Inno Setup 6（脚本见 `Installer/setup.iss`）

## 🛠 从源码构建

```bash
dotnet publish MonitoringApp.Tray/MonitoringApp.Tray.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true
```

产物位于 `MonitoringApp.Tray/bin/Release/net8.0-windows/win-x64/publish/`。也可直接运行 `publish.cmd`。

## 🔒 分发与签名

本项目的 exe 目前**未做代码签名**。未签名的程序以管理员权限运行时，接收方无法验证文件是否被篡改，因此：

- 对外分发前，建议购买 OV/EV 代码签名证书，设置 `SIGN_PFX` / `SIGN_SHA1` 等环境变量后运行 `publish.cmd`，脚本会用 signtool 自动签名并校验；
- 接收方请只从本仓库的 [Releases](https://github.com/ljiahao008-source/fan-tray/releases) 等可信渠道下载，校验文件哈希后再运行。

## 📄 许可证

本项目基于 LibreHardwareMonitor 精简而来，遵循 MPL 2.0 许可证开源。
