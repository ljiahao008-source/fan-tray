// ddcbrightness.cpp —— 可嵌入 DDC/CI 亮度模块实现
// 移植自 Twinkle Tray v1.18.0-beta2 定制版 v1.5.6 的 node-ddcci 模块，
// 只保留亮度一条通道（VCP 0x10），去掉能力字符串解析 / 其余 VCP / UI / 更新。
// 可靠性设计沿用原实现：瞬时 DDC 错误重试、句柄失效重建、静默降级。

#include "ddcbrightness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// Windows SDK 26100 起不再随带 dxva2.h（Physical Monitor API 头文件被移除），
// 但 dxva2.lib 仍存在。这里按 MSDN 声明最小必要接口，链接 dxva2.lib 即可。
#ifndef PHYSICAL_MONITOR_DESCRIPTION_SIZE
#define PHYSICAL_MONITOR_DESCRIPTION_SIZE 128
#endif
typedef struct _PHYSICAL_MONITOR {
    HANDLE hPhysicalMonitor;
    WCHAR  szPhysicalMonitorDescription[PHYSICAL_MONITOR_DESCRIPTION_SIZE];
} PHYSICAL_MONITOR;
extern "C" {
BOOL WINAPI GetNumberOfPhysicalMonitorsFromHMONITOR(HMONITOR hMonitor, LPDWORD pdwNumberOfPhysicalMonitors);
BOOL WINAPI GetPhysicalMonitorsFromHMONITOR(HMONITOR hMonitor, DWORD dwPhysicalMonitorArraySize, PHYSICAL_MONITOR* pPhysicalMonitorArray);
BOOL WINAPI DestroyPhysicalMonitor(HANDLE hMonitor);
BOOL WINAPI GetMonitorBrightness(HANDLE hMonitor, LPDWORD pdwMinimumBrightness, LPDWORD pdwCurrentBrightness, LPDWORD pdwMaximumBrightness);
BOOL WINAPI SetVCPFeature(HANDLE hMonitor, BYTE bVCPCode, DWORD dwNewValue);
}

#pragma comment(lib, "dxva2.lib")

namespace ddcb {

namespace {

constexpr int         kRetryAttempts = 3;              // 瞬时错误重试次数
constexpr auto        kRetryDelay    = std::chrono::milliseconds(50);
constexpr DWORD       kLumVcp        = 0x10;           // VCP 0x10 = LUMINANCE
constexpr ULONGLONG   kThrottleMs    = 100;            // 热键节流（防连按堆积）

// 缓存条目：物理显示器句柄 + 亮度范围（raw）
struct CachedMonitor {
    std::wstring deviceName;
    std::wstring description;
    HANDLE hPhysical = nullptr;
    int minValue = 0;
    int maxValue = 0;
    int currentValue = 0;
};

std::vector<CachedMonitor> g_cache;   // 句柄缓存（宿主单调用线程模型，模块不加锁）

// 瞬时 DDC/CI 错误（KVM/转接器/线材等偶发），值得重试；其余（不支持的
// VCP 代码、显示器已拔出等）为永久条件，直接放弃。
bool IsTransientDdcError(DWORD code) {
    switch (code) {
        case ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA:
        case ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA:
        case ERROR_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE:
        case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_COMMAND:
        case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_LENGTH:
        case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM:
            return true;
        default:
            return false;
    }
}

// 重试包装：瞬时 DDC 错误最多重试 kRetryAttempts 次（间隔 50 ms）
template <typename F>
bool TryDdc(F operation, DWORD& errorCode) {
    for (int attempt = 1; attempt <= kRetryAttempts; attempt++) {
        if (attempt > 1)
            std::this_thread::sleep_for(kRetryDelay);
        if (operation())
            return true;
        errorCode = GetLastError();
        if (!IsTransientDdcError(errorCode))
            return false;
    }
    return false;
}

std::wstring DeviceNameOf(HMONITOR h) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, &mi))
        return mi.szDevice;
    return L"";
}

// 枚举全部物理显示器并探测 DDC 亮度范围；取不到范围的（内置屏 eDP 等）
// 立即释放句柄并过滤掉 —— 静默降级，不影响宿主。
std::vector<CachedMonitor> EnumerateAll() {
    std::vector<CachedMonitor> out;
    std::vector<HMONITOR> hmons;

    auto cb = [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(h);
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(&hmons));

    for (HMONITOR hm : hmons) {
        DWORD n = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &n) || n == 0)
            continue;
        std::vector<PHYSICAL_MONITOR> pms(n);
        if (!GetPhysicalMonitorsFromHMONITOR(hm, n, pms.data()))
            continue;
        std::wstring dev = DeviceNameOf(hm);
        for (DWORD i = 0; i < n; i++) {
            CachedMonitor cm;
            cm.deviceName  = dev;
            cm.description = pms[i].szPhysicalMonitorDescription;
            cm.hPhysical   = pms[i].hPhysicalMonitor;
            DWORD minv = 0, curv = 0, maxv = 0, err = ERROR_SUCCESS;
            if (TryDdc([&] {
                    return GetMonitorBrightness(cm.hPhysical, &minv, &curv, &maxv);
                }, err) && maxv > minv) {
                cm.minValue = (int)minv;
                cm.maxValue = (int)maxv;
                cm.currentValue = (int)curv;
                out.push_back(std::move(cm));
            } else {
                DestroyPhysicalMonitor(pms[i].hPhysicalMonitor);
            }
        }
    }
    return out;
}

void DestroyAll(std::vector<CachedMonitor>& ms) {
    for (auto& m : ms) {
        if (m.hPhysical)
            DestroyPhysicalMonitor(m.hPhysical);
        m.hPhysical = nullptr;
    }
    ms.clear();
}

// 确保缓存就绪；空 = 无可用显示器或全部探测失败
bool EnsureCache() {
    if (!g_cache.empty())
        return true;
    g_cache = EnumerateAll();
    return !g_cache.empty();
}

// 设备查找：空 deviceName = 主显示器（枚举顺序第一个）
CachedMonitor* Find(const std::wstring& deviceName) {
    if (g_cache.empty())
        return nullptr;
    if (deviceName.empty())
        return &g_cache.front();
    for (auto& m : g_cache)
        if (m.deviceName == deviceName)
            return &m;
    return nullptr;
}

int RawToPercent(int raw, int min, int max) {
    return (int)std::lround((raw - min) * 100.0 / (max - min));
}

int PercentToRaw(int percent, int min, int max) {
    percent = std::clamp(percent, 0, 100);
    return (int)std::lround(min + (max - min) * percent / 100.0);
}

}  // namespace

// ── BrightnessController ─────────────────────────────────────

std::vector<MonitorInfo> BrightnessController::Enumerate() {
    std::vector<MonitorInfo> infos;
    std::vector<CachedMonitor> all = EnumerateAll();
    for (auto& cm : all) {
        MonitorInfo mi;
        mi.deviceName   = cm.deviceName;
        mi.description  = cm.description;
        mi.ddcSupported = true;
        mi.minValue     = cm.minValue;
        mi.maxValue     = cm.maxValue;
        mi.currentValue = cm.currentValue;
        mi.percent      = RawToPercent(cm.currentValue, cm.minValue, cm.maxValue);
        infos.push_back(std::move(mi));
    }
    DestroyAll(all);
    return infos;
}

bool BrightnessController::GetPercent(const std::wstring& deviceName, int& percent) {
    if (!EnsureCache())
        return false;
    CachedMonitor* m = Find(deviceName);
    if (!m)
        return false;
    DWORD minv = 0, curv = 0, maxv = 0, err = ERROR_SUCCESS;
    if (!TryDdc([&] {
            return GetMonitorBrightness(m->hPhysical, &minv, &curv, &maxv);
        }, err))
        return false;
    if (maxv <= minv)
        return false;
    percent = RawToPercent((int)curv, (int)minv, (int)maxv);
    return true;
}

bool BrightnessController::SetPercent(const std::wstring& deviceName, int percent) {
    if (!EnsureCache())
        return false;
    CachedMonitor* m = Find(deviceName);
    if (!m)
        return false;
    DWORD raw = (DWORD)PercentToRaw(percent, m->minValue, m->maxValue);
    DWORD err = ERROR_SUCCESS;
    bool ok = TryDdc([&] {
        return SetVCPFeature(m->hPhysical, kLumVcp, raw);
    }, err);
    if (!ok)
        InvalidateCache();   // 句柄可能已失效（热插拔 / 分辨率变更）
    return ok;
}

int BrightnessController::AdjustPercent(const std::wstring& deviceName, int step) {
    int cur = 0;
    if (!GetPercent(deviceName, cur))
        return -1;
    int target = std::clamp(cur + step, 0, 100);
    if (!SetPercent(deviceName, target))
        return -1;
    return target;
}

int BrightnessController::AdjustAll(int step) {
    if (!EnsureCache())
        return 0;
    int okCount = 0;
    for (auto& m : g_cache) {
        DWORD minv = 0, curv = 0, maxv = 0, err = ERROR_SUCCESS;
        if (!TryDdc([&] {
                return GetMonitorBrightness(m.hPhysical, &minv, &curv, &maxv);
            }, err))
            continue;
        if (maxv <= minv)
            continue;
        int target = std::clamp(RawToPercent((int)curv, (int)minv, (int)maxv) + step, 0, 100);
        DWORD raw = (DWORD)PercentToRaw(target, (int)minv, (int)maxv);
        DWORD err2 = ERROR_SUCCESS;
        if (TryDdc([&] {
                return SetVCPFeature(m.hPhysical, kLumVcp, raw);
            }, err2))
            okCount++;
    }
    if (okCount < (int)g_cache.size())
        InvalidateCache();   // 有台失败：全部作废，下次调用重新枚举
    return okCount;
}

int BrightnessController::SetAll(int percent) {
    if (!EnsureCache())
        return 0;
    int okCount = 0;
    for (auto& m : g_cache) {
        DWORD raw = (DWORD)PercentToRaw(percent, m.minValue, m.maxValue);
        DWORD err = ERROR_SUCCESS;
        if (TryDdc([&] {
                return SetVCPFeature(m.hPhysical, kLumVcp, raw);
            }, err))
            okCount++;
    }
    if (okCount < (int)g_cache.size())
        InvalidateCache();
    return okCount;
}

void BrightnessController::InvalidateCache() {
    DestroyAll(g_cache);
}

// ── HotkeyManager ────────────────────────────────────────────

bool HotkeyManager::Install(HWND hostWindow, AdjustHandler onAdjust,
                            UINT modifiers, UINT vkUp, UINT vkDown,
                            int stepPercent) {
    Uninstall();
    _hwnd    = hostWindow;
    _mods    = modifiers;
    _vkUp    = vkUp;
    _vkDown  = vkDown;
    _step    = stepPercent > 0 ? stepPercent : 10;
    _onAdjust = std::move(onAdjust);
    _lastTick = 0;

    bool upOk = RegisterHotKey(hostWindow, kHotkeyUp, modifiers, vkUp);
    bool dnOk = RegisterHotKey(hostWindow, kHotkeyDown, modifiers, vkDown);
    if (!upOk || !dnOk) {
        Uninstall();   // 有一个被占用即整体回滚
        return false;
    }
    return true;
}

void HotkeyManager::Uninstall() {
    if (_hwnd) {
        UnregisterHotKey(_hwnd, kHotkeyUp);
        UnregisterHotKey(_hwnd, kHotkeyDown);
    }
    _hwnd = nullptr;
    _onAdjust = nullptr;
}

bool HotkeyManager::HandleMessage(UINT msg, WPARAM wp) {
    if (msg != WM_HOTKEY || !_hwnd || !_onAdjust)
        return false;
    int step = 0;
    if (wp == kHotkeyUp)
        step = _step;
    else if (wp == kHotkeyDown)
        step = -_step;
    else
        return false;

    ULONGLONG now = GetTickCount64();
    if (now - _lastTick < kThrottleMs)
        return true;   // 100 ms 节流：连按不堆积、不丢拍
    _lastTick = now;
    _onAdjust(step);
    return true;
}

}  // namespace ddcb
