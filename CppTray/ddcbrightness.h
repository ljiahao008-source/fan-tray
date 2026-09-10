#pragma once
// ─────────────────────────────────────────────────────────────
// ddcbrightness.h —— 可嵌入 DDC/CI 亮度模块（Twinkle Tray 精简移植）
// 只做一件事：对外接显示器做 DDC/CI（VCP 0x10）亮度调节 + 全局快捷键。
// 无 UI、无自有消息循环、无 gamma ramp 触碰 —— 与 LightBulb 正交共存。
// 依赖：仅系统库 dxva2.lib + user32.lib。
// ─────────────────────────────────────────────────────────────
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace ddcb {

// 单个显示器信息
struct MonitorInfo {
    std::wstring deviceName;        // "\\.\DISPLAY1"（稳定标识，用作 key）
    std::wstring description;       // 物理显示器描述（EDID 名称，可空）
    bool  ddcSupported = false;     // 是否成功取到亮度范围（DDC/CI 能力）
    int   minValue = 0;             // 硬件亮度最小值
    int   maxValue = 100;           // 硬件亮度最大值
    int   currentValue = 0;         // 当前硬件亮度原始值
    int   percent = 0;              // 当前亮度百分比（0-100）
};

// 亮度控制器（全部为静态方法：无实例状态，适配"单调用线程"模型）
class BrightnessController {
public:
    // 枚举当前所有显示器（每次重新枚举，天然支持热插拔）
    static std::vector<MonitorInfo> Enumerate();

    // 读取亮度百分比；deviceName 为空 = 主显示器。成功返回 true
    static bool GetPercent(const std::wstring& deviceName, int& percent);

    // 设置亮度百分比（0-100，内部钳制）
    static bool SetPercent(const std::wstring& deviceName, int percent);

    // 相对调整：step 为百分比（可负）。返回调整后百分比，失败返回 -1
    static int  AdjustPercent(const std::wstring& deviceName, int step);

    // 批量：对所有支持 DDC 的显示器做相对调整，返回成功的显示器台数
    static int  AdjustAll(int step);

    // 批量：对所有支持 DDC 的显示器设同一百分比
    static int  SetAll(int percent);

    // 释放内部缓存的物理显示器句柄（宿主退出/设备变更时调用）
    static void InvalidateCache();
};

// 快捷键管理：注册两个热键，解析 WM_HOTKEY 后回调步进值
class HotkeyManager {
public:
    using AdjustHandler = std::function<void(int stepPercent)>;   // +step / -step

    // 安装：hostWindow 为宿主窗口（用于接收 WM_HOTKEY）
    bool Install(HWND hostWindow, AdjustHandler onAdjust,
                 UINT modifiers = MOD_CONTROL | MOD_ALT,
                 UINT vkUp = VK_UP, UINT vkDown = VK_DOWN,
                 int  stepPercent = 10);

    void Uninstall();                       // 注销两个热键

    // 宿主消息循环内调用：msg==WM_HOTKEY 且属于本模块时返回 true（已处理）
    bool HandleMessage(UINT msg, WPARAM wp);

    static constexpr int kHotkeyUp   = 0xB001;   // 模块占用的热键 ID 段
    static constexpr int kHotkeyDown = 0xB002;

private:
    HWND _hwnd = nullptr;
    UINT _mods = 0, _vkUp = 0, _vkDown = 0;
    int  _step = 10;
    AdjustHandler _onAdjust;
    ULONGLONG _lastTick = 0;   // 节流基准（100 ms）
};

}  // namespace ddcb
