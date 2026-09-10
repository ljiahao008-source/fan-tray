#pragma once
// 设置对话框（极简 Win32 模态框）：刷新间隔 + 开机自启
// 对照 SettingsWindow（WPF 版）

#include <windows.h>
#include "config.h"

// 模态打开设置对话框。返回 true 表示用户点了"确定"且配置已写入。
// inout cfg：调用前传入当前配置，确定后传出新配置。
bool ShowSettingsDialog(HWND parent, AppConfig& cfg);
