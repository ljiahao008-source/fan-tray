#pragma once
// mainwindow.h —— 主程序交互窗口
// 上部实时监控数值，底部操作按钮（设置/锁屏设置/重置统计/退出），
// 点击按钮发 kActionMsg 给宿主隐藏主窗口处理；关闭窗口 = 隐藏回托盘。

#include <windows.h>
#include "monitor.h"
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

// 注册窗口类并创建（隐藏）。启动时调用一次。
bool Create(HINSTANCE hInst);

// 设置宿主隐藏主窗口句柄（用于转发按钮动作），Create 后调用。
void SetHost(HWND hostHiddenMain);

// 居中显示（从托盘/菜单唤起时调用）
void Show();

// 隐藏回托盘
void Hide();

bool IsVisible();
HWND Hwnd();

// 数据刷新（宿主 kSampleMsg 调用，UI 线程）
void Update(const SampleSet& s);

// 显示项配置（设置保存/启动时调用）
void ApplyConfig(const AppConfig& cfg);

// 阈值（与挂件一致，采样就绪后宿主传入）
void SetThresholds(const Thresholds& thr);

// 销毁窗口
void DestroyWindowW();

}  // namespace mainwin
