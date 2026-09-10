#pragma once
// mainwindow.h —— 主程序窗口（监控仪表盘）
// 只展示实时监控数值（功耗/风扇/占用/温度/内存/网速），不承载设置；
// 设置仍从托盘菜单进入。关闭窗口 = 隐藏回托盘。

#include <windows.h>
#include "monitor.h"
#include "config.h"

namespace mainwin {

// 注册窗口类并创建（隐藏）。启动时调用一次。
bool Create(HINSTANCE hInst);

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
