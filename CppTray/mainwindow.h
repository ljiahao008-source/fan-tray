#pragma once
// mainwindow.h —— 主程序全功能面板（Twinkle Tray 原版外观，全自绘 GDI+）
// 覆盖：硬件监控数值 + 每台显示器（亮度/对比度/音量滑块、输入源、电源）
//      + 色温护眼（状态与微调按钮）+ 底部操作（设置/锁屏设置/重置统计/退出）

#include <windows.h>
#include "config.h"
#include "monitor.h"

namespace mainwin {

// 面板按钮动作（点击 → 宿主处理）
enum Action : UINT {
    ActionSettings = 1,     // 打开设置对话框
    ActionLockScreen = 2,   // 打开锁屏设置窗口
    ActionReset = 3,        // 重置统计
    ActionExit = 4,         // 退出程序
    ActionTempWarm = 5,     // 色温 +
    ActionTempCool = 6,     // 色温 −
    ActionTempToggle = 7,   // 暂停/恢复色温
};

// 按钮动作消息：面板 PostMessage 到宿主隐藏主窗口，wParam = Action
constexpr UINT kActionMsg = WM_APP + 12;

bool Create(HINSTANCE hInst);
void SetHost(HWND hostHiddenMain);
void Show();
void Hide();
bool IsVisible();
HWND Hwnd();

// 每秒刷新：监控数值 + DDC 面板数值（宿主 kSampleMsg 调用）
void Refresh(const SampleSet& s);

// 色温状态注入（宿主查询 colortemp 后调用）
void SetColorTemp(bool enabled, int kelvin);

// 面板重建（显示器热插拔后 / 设置保存后调用）
void Rebuild();

void DestroyWindowW();

}  // namespace mainwin
