#pragma once
// ─────────────────────────────────────────────────────────────
// colortemp.h —— 可嵌入色温护眼模块（LightBulb 算法移植，全功能核心）
// 功能：按日出/日落时间平滑过渡屏幕色温（gamma ramp）+ 全局热键
//   · 色温→RGB：Tanner Helland 算法（与 LightBulb 一致）
//   · 调度：余弦/正弦过渡曲线（与 LightBulb Cycle 一致）
//   · 应用：SetDeviceGammaRamp 线性 ramp × 通道倍数 + 微扰动防驱动忽略
//   · 热键：±色温、暂停/恢复（RegisterHotKey → WM_HOTKEY）
// 与 DDC 亮度模块正交：DDC 写显示器寄存器，本模块写 gamma ramp，互不干扰。
// ─────────────────────────────────────────────────────────────
#include <windows.h>
#include <string>
#include <vector>

namespace colortemp {

// 色温配置（宿主提供；小时/分钟用"自 0:00 起的分钟数"）
struct Settings {
    bool enabled = false;        // 总开关
    int  dayTemperature = 6500;  // 白天色温（K）
    int  nightTemperature = 4500;// 夜间色温（K）
    int  sunriseMinutes = 360;   // 日出 06:00
    int  sunsetMinutes = 1140;   // 日落 19:00
    int  transitionMinutes = 60; // 过渡时长（分钟）
    double transitionOffset = 0.5;  // 过渡相位 0~1
    int  hotkeyStepK = 500;      // 热键每次步进（K）
};

// 纯函数：给定配置与"当日分钟数(0..1439)"，返回当前目标色温（K）。
// 独立可测，线程安全。
int ComputeTemperature(const Settings& s, int minuteOfDay);

// 色温管理器：自带后台线程，每秒轮询调度并应用 gamma。
class ColorTemperatureManager {
public:
    ColorTemperatureManager();   // 初始化临界区（生命周期随对象）
    ~ColorTemperatureManager();  // Stop + 释放临界区

    // 启动：hostWindow 用于接收 WM_HOTKEY（须先 ApplySettings）
    bool Start(HWND hostWindow);

    // 停止：恢复所有显示器 gamma（1,1,1）并释放资源
    void Stop();

    // 热更新配置（设置保存后调用）
    void ApplySettings(const Settings& s);

    // 热键动作：±K（叠加在调度结果上）、暂停/恢复。返回当前生效色温，失败 -1。
    int AdjustTemperature(int deltaK);
    void TogglePause();

    // 查询当前状态（锁内快照）：enabled=总开关+未暂停；kelvin=当前生效色温
    void GetState(bool& enabled, int& kelvin) const;

    // 宿主消息循环内调用：msg==WM_HOTKEY 且属于本模块时返回 true
    bool HandleHotkey(UINT msg, WPARAM wp);

    bool IsRunning() const { return _thread != nullptr; }

    static constexpr int kHotkeyWarm   = 0xB011;   // 色温 +（更冷/白）
    static constexpr int kHotkeyCool   = 0xB012;   // 色温 −（更暖/黄）
    static constexpr int kHotkeyToggle = 0xB013;   // 暂停/恢复

private:
    static DWORD WINAPI ThreadProc(LPVOID p);
    void Tick();                       // 计算目标色温并应用 gamma
    void ApplyGammaLocked();           // 锁内执行（当前目标 → ramp）
    void ResetGammaLocked();           // 恢复 1,1,1

    HWND _host = nullptr;
    HANDLE _thread = nullptr;
    HANDLE _wakeEvent = nullptr;       // 热键/设置改动即时唤醒
    CRITICAL_SECTION _lock;
    bool _csInited = false;            // 临界区已初始化（Stop 幂等守卫）
    bool _running = false;
    Settings _settings;
    bool _paused = false;
    int  _offsetK = 0;                 // 热键叠加偏移
    int  _lastAppliedK = -1;           // 上次生效色温（-1=尚未生效）
    std::vector<std::pair<std::wstring, HDC>> _dcs;   // 显示器 DC 缓存
};

}  // namespace colortemp
