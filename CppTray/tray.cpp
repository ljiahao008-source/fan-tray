#include "tray.h"

#include "autostart.h"

#pragma comment(lib, "shell32.lib")

namespace {
constexpr UINT kTrayId = 1;
}  // namespace

// 由 main.cpp 设置的回调
extern "C" void TrayMenuCallback(UINT id, bool checked, HWND hwnd);

bool TrayIcon::Create(HWND hwnd, HINSTANCE hInst) {
    _hwnd = hwnd;

    // 菜单
    _menu = CreatePopupMenu();
    AppendMenuW(_menu, MF_STRING, kMenuSettings, L"设置");
    AppendMenuW(_menu, MF_STRING, kMenuReset, L"重置统计");
    AppendMenuW(_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(_menu, MF_STRING, kMenuAutoStart, L"开机自启");
    AppendMenuW(_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(_menu, MF_STRING, kMenuExit, L"退出");

    // 图标（取 exe 自带图标）
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    _icon = ExtractIconW(hInst, exe, 0);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon = _icon ? _icon : LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"机械革命监控（C++ 版）");
    Shell_NotifyIconW(NIM_ADD, &nid);
    return true;
}

void TrayIcon::SetTooltip(const wchar_t* text) {
    if (_tip == text)
        return;   // 文本没变不写，省 COM 调用
    _tip = text;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowMenu(HWND hwnd) {
    // 每次打开同步自启勾选状态
    bool on = IsAutoStartTaskInstalled();
    CheckMenuItem(_menu, kMenuAutoStart, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(_menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);   // 菜单关闭后任务栏焦点恢复

    if (cmd == 0)
        return;

    bool checked = false;
    if (cmd == kMenuAutoStart)
        checked = (GetMenuState(_menu, kMenuAutoStart, MF_BYCOMMAND) & MF_CHECKED) != 0;

    TrayMenuCallback(cmd, checked, hwnd);
}

void TrayIcon::Remove() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (_icon)
        DestroyIcon(_icon);
    if (_menu)
        DestroyMenu(_menu);
}
