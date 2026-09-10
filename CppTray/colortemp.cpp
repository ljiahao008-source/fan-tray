// colortemp.cpp —— 可嵌入色温护眼模块（LightBulb 算法移植）
// 算法来源：LightBulb（MIT, Tyrrrz）GammaService / DeviceContext / Cycle：
//   · 色温→RGB：Tanner Helland（tannerhelland.com/4435/convert-temperature-rgb-algorithm-code）
//   · 过渡曲线：余弦/正弦（夜→昼 / 昼→夜）
//   · gamma 应用：线性 ramp × 通道倍数，末尾微扰动强制驱动刷新

#include "colortemp.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace colortemp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMinPerDay = 24 * 60;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
double Clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
int WrapMin(int m) {   // 归一到 [0, 1440)
    m %= kMinPerDay;
    return m < 0 ? m + kMinPerDay : m;
}

// ── Tanner Helland 色温→通道倍数（与 LightBulb GammaService 一致）──
double GetRed(int t) {
    if (t > 6600)
        return Clamp01(std::pow(t / 100.0 - 60, -0.1332047592) * 329.698727446 / 255);
    return 1.0;
}
double GetGreen(int t) {
    if (t > 6600)
        return Clamp01(std::pow(t / 100.0 - 60, -0.0755148492) * 288.1221695283 / 255);
    return Clamp01((std::log(t / 100.0) * 99.4708025861 - 161.1195681661) / 255);
}
double GetBlue(int t) {
    if (t >= 6600)
        return 1.0;
    if (t <= 1900)
        return 0.0;
    return Clamp01((std::log(t / 100.0 - 10) * 138.5177312231 - 305.0447927307) / 255);
}

struct GammaRamp {
    unsigned short red[256], green[256], blue[256];
};

// 线性 ramp × 倍数 + 末端微扰动（防止驱动因"与上次相同"而忽略更新）
bool ApplyGammaRamp(HDC dc, double rm, double gm, double bm) {
    static int s_offset = 0;
    GammaRamp ramp;
    for (int i = 0; i < 256; i++) {
        ramp.red[i]   = (unsigned short)(i * 255 * rm);
        ramp.green[i] = (unsigned short)(i * 255 * gm);
        ramp.blue[i]  = (unsigned short)(i * 255 * bm);
    }
    s_offset = (s_offset + 1) % 5;
    ramp.red[255]   = (unsigned short)(ramp.red[255] + s_offset);
    ramp.green[255] = (unsigned short)(ramp.green[255] + s_offset);
    ramp.blue[255]  = (unsigned short)(ramp.blue[255] + s_offset);
    return SetDeviceGammaRamp(dc, &ramp) != FALSE;
}

// 枚举当前显示器设备名列表
std::vector<std::wstring> EnumerateDeviceNames() {
    std::vector<std::wstring> out;
    auto cb = [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(h, &mi) && mi.szDevice[0])
            reinterpret_cast<std::vector<std::wstring>*>(lp)->push_back(mi.szDevice);
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(&out));
    return out;
}

}  // namespace

// ── 调度纯函数（LightBulb Cycle 数学，分钟制）──
// prev/next 语义：prevX = 最近一次"≤now"的出现；nextX = 最近一次">now"的出现
// （均为可跨 0/1440 的绝对分钟，用于跨午夜处理）
int ComputeTemperature(const Settings& s, int minuteOfDay) {
    int dayK = Clamp(s.dayTemperature, 1000, 10000);
    int nightK = Clamp(s.nightTemperature, 1000, 10000);
    int dur = std::max(s.transitionMinutes, 1);
    double off = Clamp01(s.transitionOffset);

    int sr = WrapMin(s.sunriseMinutes);
    int ss = WrapMin(s.sunsetMinutes);
    int now = WrapMin(minuteOfDay);

    int srStart = WrapMin(sr - (int)std::lround(dur * (1 - off)));
    int srEnd   = WrapMin(sr + (int)std::lround(dur * off));
    int ssStart = WrapMin(ss - (int)std::lround(dur * off));
    int ssEnd   = WrapMin(ss + (int)std::lround(dur * (1 - off)));

    // 日出过渡（夜→昼，余弦）
    int prevSrStart = srStart <= now ? srStart : srStart - kMinPerDay;
    int nextSrEnd   = srEnd > now ? srEnd : srEnd + kMinPerDay;
    if (nextSrEnd - prevSrStart <= dur) {
        double p = (now - prevSrStart) / (double)dur;
        return (int)std::lround(dayK + (nightK - dayK) * std::cos(p * kPi / 2));
    }

    // 日落过渡（昼→夜，正弦）
    int prevSsStart = ssStart <= now ? ssStart : ssStart - kMinPerDay;
    int nextSsEnd   = ssEnd > now ? ssEnd : ssEnd + kMinPerDay;
    if (nextSsEnd - prevSsStart <= dur) {
        double p = (now - prevSsStart) / (double)dur;
        return (int)std::lround(dayK + (nightK - dayK) * std::sin(p * kPi / 2));
    }

    // 白天/夜晚：下一次日落结束先到 = 白天
    int nextSrEnd2 = srEnd > now ? srEnd : srEnd + kMinPerDay;
    int nextSsEnd2 = ssEnd > now ? ssEnd : ssEnd + kMinPerDay;
    return nextSsEnd2 < nextSrEnd2 ? dayK : nightK;
}

// ── ColorTemperatureManager ──────────────────────────────────

ColorTemperatureManager::ColorTemperatureManager() {
    InitializeCriticalSection(&_lock);
    _csInited = true;
}

ColorTemperatureManager::~ColorTemperatureManager() {
    Stop();
    if (_csInited) {
        DeleteCriticalSection(&_lock);
        _csInited = false;
    }
}

bool ColorTemperatureManager::Start(HWND hostWindow) {
    Stop();

    _host = hostWindow;
    _running = true;
    _paused = false;
    _offsetK = 0;
    _lastAppliedK = -1;
    _wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _thread = CreateThread(nullptr, 0, &ThreadProc, this, 0, nullptr);
    if (!_thread) {
        _running = false;
        if (_wakeEvent) { CloseHandle(_wakeEvent); _wakeEvent = nullptr; }
        DeleteCriticalSection(&_lock);
        return false;
    }

    // 热键注册失败不阻塞调度功能（注册结果由宿主按需感知）
    RegisterHotKey(hostWindow, kHotkeyWarm, MOD_CONTROL | MOD_ALT, VK_PRIOR);
    RegisterHotKey(hostWindow, kHotkeyCool, MOD_CONTROL | MOD_ALT, VK_NEXT);
    RegisterHotKey(hostWindow, kHotkeyToggle, MOD_CONTROL | MOD_ALT, VK_HOME);
    return true;
}

void ColorTemperatureManager::Stop() {
    if (_thread) {
        _running = false;
        SetEvent(_wakeEvent);
        WaitForSingleObject(_thread, 2000);
        CloseHandle(_thread);
        _thread = nullptr;
    }
    if (_host) {
        UnregisterHotKey(_host, kHotkeyWarm);
        UnregisterHotKey(_host, kHotkeyCool);
        UnregisterHotKey(_host, kHotkeyToggle);
        _host = nullptr;
    }
    if (_wakeEvent) {
        CloseHandle(_wakeEvent);
        _wakeEvent = nullptr;
    }
    // 恢复 gamma 并释放 DC
    ResetGammaLocked();
}

void ColorTemperatureManager::ApplySettings(const Settings& s) {
    EnterCriticalSection(&_lock);
    _settings = s;
    _offsetK = 0;   // 配置变更时清除热键偏移
    LeaveCriticalSection(&_lock);
    if (_wakeEvent)
        SetEvent(_wakeEvent);
}

int ColorTemperatureManager::AdjustTemperature(int deltaK) {
    EnterCriticalSection(&_lock);
    if (!_running) {
        LeaveCriticalSection(&_lock);
        return -1;
    }
    _offsetK += deltaK;
    int target = Clamp(ComputeTemperature(_settings, WrapMin((int)(GetTickCount64() / 60000) % kMinPerDay)) + _offsetK, 1000, 10000);
    LeaveCriticalSection(&_lock);
    if (_wakeEvent)
        SetEvent(_wakeEvent);
    return target;
}

void ColorTemperatureManager::TogglePause() {
    EnterCriticalSection(&_lock);
    _paused = !_paused;
    LeaveCriticalSection(&_lock);
    if (_wakeEvent)
        SetEvent(_wakeEvent);
}

bool ColorTemperatureManager::HandleHotkey(UINT msg, WPARAM wp) {
    if (msg != WM_HOTKEY)
        return false;
    if (wp == kHotkeyWarm) {
        EnterCriticalSection(&_lock);
        int step = _settings.hotkeyStepK > 0 ? _settings.hotkeyStepK : 500;
        LeaveCriticalSection(&_lock);
        AdjustTemperature(step);
        return true;
    }
    if (wp == kHotkeyCool) {
        EnterCriticalSection(&_lock);
        int step = _settings.hotkeyStepK > 0 ? _settings.hotkeyStepK : 500;
        LeaveCriticalSection(&_lock);
        AdjustTemperature(-step);
        return true;
    }
    if (wp == kHotkeyToggle) {
        TogglePause();
        return true;
    }
    return false;
}

DWORD WINAPI ColorTemperatureManager::ThreadProc(LPVOID p) {
    ColorTemperatureManager* self = (ColorTemperatureManager*)p;
    while (self->_running) {
        WaitForSingleObject(self->_wakeEvent, 1000);   // 每秒轮询，热键/设置即时唤醒
        self->Tick();
    }
    return 0;
}

void ColorTemperatureManager::Tick() {
    EnterCriticalSection(&_lock);
    ApplyGammaLocked();
    LeaveCriticalSection(&_lock);
}

// 锁内调用：根据当前状态计算目标色温并应用到所有显示器
void ColorTemperatureManager::ApplyGammaLocked() {
    // 显示器列表变化时重建 DC 缓存
    std::vector<std::wstring> devices = EnumerateDeviceNames();
    bool same = devices.size() == _dcs.size();
    if (same) {
        for (size_t i = 0; i < devices.size(); i++)
            if (devices[i] != _dcs[i].first) { same = false; break; }
    }
    if (!same) {
        ResetGammaLocked();   // 旧 DC 先复位
        _dcs.clear();
        for (auto& dev : devices) {
            HDC dc = CreateDCW(dev.c_str(), dev.c_str(), nullptr, nullptr);
            if (dc)
                _dcs.emplace_back(dev, dc);
        }
    }
    if (_dcs.empty())
        return;

    int target = 0;
    if (!_settings.enabled || _paused) {
        // 暂停/关闭：恢复中性 gamma（若此前不是中性）
        if (_lastAppliedK >= 0) {
            for (auto& d : _dcs)
                ApplyGammaRamp(d.second, 1.0, 1.0, 1.0);
            _lastAppliedK = -1;
        }
        return;
    }

    int minuteOfDay = WrapMin((int)(GetTickCount64() / 60000) % kMinPerDay);
    target = Clamp(ComputeTemperature(_settings, minuteOfDay) + _offsetK, 1000, 10000);

    // 与 LightBulb 一致：变化 <15K 不重写（避免闪烁/卡顿）
    if (_lastAppliedK >= 0 && std::abs(target - _lastAppliedK) < 15)
        return;

    double rm = GetRed(target), gm = GetGreen(target), bm = GetBlue(target);
    for (auto& d : _dcs)
        ApplyGammaRamp(d.second, rm, gm, bm);
    _lastAppliedK = target;
}

void ColorTemperatureManager::ResetGammaLocked() {
    for (auto& d : _dcs)
        ApplyGammaRamp(d.second, 1.0, 1.0, 1.0);
    for (auto& d : _dcs)
        DeleteDC(d.second);
    _dcs.clear();
    _lastAppliedK = -1;
}

}  // namespace colortemp
