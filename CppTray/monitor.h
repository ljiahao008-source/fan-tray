#pragma once
// 采样核心：CPU 功耗（PawnIO MSR RAPL）+ 风扇转速（ACPI WMI）
//           + CPU 占用（GetSystemTimes）+ CPU 温度（PawnIO SMN）+ 内存（GlobalMemoryStatusEx）
//           + 网速上下行（GetIfTable2 差分）+ 统计
// 算法对照 LibreHardwareMonitor RAPL/Amd17Cpu 实现（MPL-2.0）

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
    double usageElevated = 70, usageCritical = 90;   // CPU 占用 %
    double tempElevated = 75, tempCritical = 90;     // CPU 温度 °C
    double memElevated = 70, memCritical = 90;       // 内存占用 %
};

// 一次采样的全部指标快照（网速拆上下行两个）
struct SampleSet {
    Metric power, fan, cpuUsage, cpuTemp, mem, netDown, netUp;
};

class MonitorCore {
public:
    MonitorCore();
    ~MonitorCore();

    // 初始化 PawnIO 驱动 + WMI。失败时对应指标显示 "--"。
    bool Init();

    // 单次采样并更新全部指标统计。
    void Sample(SampleSet& out);

    // 重置最低/最高/平均（UI 线程调用，与采样互斥）。
    void ResetStats();

    // 阈值：功耗按 CPU 型号估算 TDP；占用/温度/内存为通用经验值。
    Thresholds GetThresholds();

    // 功耗自愈：拉起 PawnIO 驱动服务并重建功耗引擎。
    void Revive();

private:
    bool SetupPawnPower();                  // 尝试 Intel/AMD RAPL
    bool ReadPackagePower(float& watts);
    bool ReadCpuUsage(float& pct);          // GetSystemTimes 差分
    bool ReadCpuTemp(float& celsius);       // AMD SMN THM_TCON_CUR_TMP
    bool ReadNetSpeeds(float& downKBps, float& upKBps);   // GetIfTable2 差分
    static bool ReadMemUsage(float& pct);   // GlobalMemoryStatusEx
    static double EstimateTdp(const std::wstring& name);

    CRITICAL_SECTION _lock;
    PawnIo _pawn;
    bool _pawnOk = false;
    bool _isAmd = false;                    // 温度仅 AMD 走 SMN；Intel 暂不支持
    uint32_t _pkgMsr = 0x611;
    double _mult = 0;                       // 能耗单位换算 (J/计数)
    uint32_t _lastEnergy = 0;
    LARGE_INTEGER _lastTime{};
    bool _hasLastEnergy = false;
    LARGE_INTEGER _freq{};

    // CPU 占用基准
    ULONGLONG _lastIdle = 0, _lastKernel = 0, _lastUser = 0;
    bool _hasLastSysTimes = false;

    // 网速基准
    ULONGLONG _lastNetIn = 0, _lastNetOut = 0;
    LARGE_INTEGER _lastNetTime{};
    bool _hasLastNet = false;

    Metric _power, _fan, _cpuUsage, _cpuTemp, _mem, _netDown, _netUp;   // 内部统计快照

    MechrevoFan _fanDev;
    float _lastFan = 0.f;
    bool _hasLastFan = false;
    int _fanFailStreak = 0;
    ULONGLONG _powerDeadSinceTick = 0;   // 0 = 未开始计
};
