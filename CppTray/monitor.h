#pragma once
// 采样核心：CPU 功耗（PawnIO MSR RAPL）+ 风扇转速（ACPI WMI）+ 统计
// 算法对照 MonitorCore.cs 与 LibreHardwareMonitor RAPL 实现

#include <windows.h>
#include <string>
#include "pawnio.h"
#include "wmifan.h"

// 单指标统计：当前值 + 最低/最高/平均
struct Metric {
    float current = 0.f, min = 0.f, max = 0.f, avg = 0.f;
    bool valid = false;   // 当前值是否有效
    double sum = 0;       // 累计和（精确平均）
    int count = 0;
    void Reset() { current = min = max = avg = 0.f; valid = false; sum = 0; count = 0; }
};

struct Thresholds {
    double powerElevated = 45, powerCritical = 72;
    double fanElevated = 3200, fanCritical = 4800;
};

class MonitorCore {
public:
    MonitorCore();
    ~MonitorCore();

    // 初始化 PawnIO 驱动 + WMI。失败时功耗/风扇显示 "--"。
    bool Init();

    // 单次采样并更新统计（功耗死亡自愈触发时返回 false 表示本拍无功耗）。
    void Sample(Metric& power, Metric& fan);

    // 重置最低/最高/平均（UI 线程调用，与采样互斥）。
    void ResetStats();

    // 按 CPU 型号后缀估算阈值（HX/HK≈55W、H/HS/HQ≈45W、P≈28W、U≈15W，未知 35W）。
    Thresholds GetThresholds();

    // 功耗自愈：拉起 PawnIO 驱动服务并重建功耗引擎。
    void Revive();

private:
    bool SetupPawnPower();          // 尝试 Intel/AMD RAPL
    bool ReadPackagePower(float& watts);
    static double EstimateTdp(const std::wstring& name);
    CRITICAL_SECTION _lock;
    PawnIo _pawn;
    bool _pawnOk = false;
    uint32_t _pkgMsr = 0x611;
    double _mult = 0;               // 能耗单位换算 (J/计数)
    uint32_t _lastEnergy = 0;
    LARGE_INTEGER _lastTime{};
    bool _hasLastEnergy = false;
    LARGE_INTEGER _freq{};

    Metric _power, _fan;            // 内部统计快照

    MechrevoFan _fanDev;
    float _lastFan = 0.f;
    bool _hasLastFan = false;
    int _fanFailStreak = 0;
    ULONGLONG _powerDeadSinceTick = 0;   // 0 = 未开始计
};
