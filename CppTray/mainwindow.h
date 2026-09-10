#pragma once
// mainwindow.h —— 主程序交互窗口（Twinkle Tray 式显示器控制面板）
// 每台显示器：亮度/对比度/音量滑块、输入源、电源；
// 底部操作按钮（设置/锁屏设置/重置统计/退出）。
// 关闭窗口 = 隐藏回托盘。

#include <windows.h>
#include "config.h"

namespace mainwin {

// 主窗口动作（按钮点击 → 宿主处理）
enum Action : UINT {
    ActionSettings = 1,     // 打开设置对话框
    ActionLockScreen = 2,   // 打开锁屏设置窗口
    ActionReset = 3,        // 重置统计
    ActionExit = 4,         // 退出程序
};

// 按钮动作消息：主窗口 PostMessage 到宿主隐藏主窗口，wParam = Action
constexpr UINT kActionMsg = WM_APP + 12;

bool Create(HINSTANCE hInst);
void SetHost(HWND hostHiddenMain);
void Show();
void Hide();
bool IsVisible();
HWND Hwnd();

// 定时刷新面板数值（宿主每秒调用；内部仅在可见时执行）
void Refresh();

// 面板重建（显示器热插拔后 / 设置保存后调用）
void Rebuild();

void DestroyWindowW();

}  // namespace mainwin
