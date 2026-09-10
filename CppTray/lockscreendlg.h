#pragma once
// 锁屏设置窗口（Win32 原生控件，模态）
// 三页：电源设置 / 屏幕保护 / 动态锁；底部状态栏与操作提示
// 由挂件托盘菜单调用（与设置对话框同一模式：模态 + 禁用父窗口）

#include <windows.h>

namespace lockscreen {

// 打开锁屏设置窗口（模态，返回后父窗口恢复可用）
void ShowLockScreenWindow(HWND parent);

}  // namespace lockscreen
