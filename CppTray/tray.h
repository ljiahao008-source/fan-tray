#pragma once
// 托盘图标 + 右键菜单 + ToolTip（对照 TrayController.cs 的托盘部分）

#include <windows.h>
#include <string>

// 托盘回调消息（发给主窗口）
constexpr UINT kTrayCallbackMsg = WM_APP + 10;

// 菜单项 ID（main.cpp 的 TrayMenuCallback 使用）
constexpr UINT kMenuOpenMain = 0;    // 打开主窗口
constexpr UINT kMenuSettings = 1;
constexpr UINT kMenuReset = 2;
constexpr UINT kMenuAutoStart = 3;
constexpr UINT kMenuExit = 4;
constexpr UINT kMenuLockScreen = 5;   // 锁屏设置窗口

class TrayIcon {
public:
    bool Create(HWND hwnd, HINSTANCE hInst);
    void SetTooltip(const wchar_t* text);       // 文本没变不写，省 COM 调用
    void ShowMenu(HWND hwnd);                   // 弹出右键菜单
    void Remove();

private:
    HWND _hwnd = nullptr;
    HICON _icon = nullptr;
    HMENU _menu = nullptr;
    std::wstring _tip;
};
