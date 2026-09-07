// 精简监控核心：仅 CPU 功耗（LibreHardwareMonitor）+ 风扇转速（机械革命私有 EC）。

using System;
using LibreHardwareMonitor.Hardware;
using LibreHardwareMonitor.Mechrevo;

namespace MonitoringApp.Tray;

/// <summary>单指标统计：当前值 + 最低 / 最高 / 平均（与原版 Metric 同算法）。</summary>
public sealed class Metric
{
    private double _sum;
    private long _count;

    public float? Current { get; internal set; }
    public float? Min { get; internal set; }
    public float? Max { get; internal set; }
    public float? Average { get; internal set; }

    internal void Update(float? value)
    {
        Current = value;
        if (value is not { } v)
            return;

        Min = Min is null ? v : Math.Min(Min.Value, v);
        Max = Max is null ? v : Math.Max(Max.Value, v);
        _sum += v;
        _count++;
        Average = (float)(_sum / _count);
    }

    internal void Reset()
    {
        Current = Min = Max = Average = null;
        _sum = 0;
        _count = 0;
    }
}

/// <summary>两指标监控快照：CPU 功耗、风扇转速（各含最低/最高/平均）。</summary>
public sealed class MonitoringSnapshot
{
    public Metric CpuPower { get; } = new();
    public Metric FanRpm { get; } = new();
}

/// <summary>
/// 精简监控服务：每次 Read() 更新两项指标。
/// · CPU 功耗：LHM CPU 传感器的 Package/Total Power；
/// · 风扇转速：机械革命私有 ACPI WMI（PowerSwitchInterface），带 3 秒缓存降低 WMI 调用开销。
/// 资源优化：CPU 硬件与功耗传感器对象跨更新稳定存在，首次找到后缓存引用，
/// 每秒只做一次硬件 Update + 一次传感器取值，免去每秒枚举硬件树和传感器名字字符串匹配；
/// 统计区加锁，保证后台采样线程与 UI 线程的"重置统计"互不踩踏。
/// </summary>
public sealed class MonitorCore : IDisposable
{
    private const double FanCacheSeconds = 3.0;

    private readonly Computer _computer;
    private readonly MechrevoEcProvider _ec;
    private readonly MonitoringSnapshot _snapshot = new();
    private readonly object _statsLock = new();
    private DateTime _lastFanRead = DateTime.MinValue;
    private float? _cachedFanRpm;
    private IHardware? _cachedCpu;
    private ISensor? _cachedPowerSensor;

    public MonitorCore()
    {
        _computer = new Computer
        {
            IsCpuEnabled = true,
            IsGpuEnabled = false,
            IsMemoryEnabled = false,
            IsMotherboardEnabled = false,
        };
        _computer.Open();
        _ec = new MechrevoEcProvider();
    }

    /// <summary>单次读取并更新两指标快照。耗时的硬件 Update / WMI 在锁外执行。</summary>
    public MonitoringSnapshot Read()
    {
        float? cpuPower = null;

        if (_cachedCpu is null)
        {
            foreach (IHardware hw in _computer.Hardware)
            {
                if (hw.HardwareType == HardwareType.Cpu)
                {
                    _cachedCpu = hw;
                    break;
                }
            }
        }

        if (_cachedCpu is { } cpu)
        {
            cpu.Update();
            _cachedPowerSensor ??= FindPowerSensor(cpu);
            if (_cachedPowerSensor is { } sensor)
                cpuPower = sensor.Value;
        }

        float? fanRpm = ReadFanRpmCached();   // WMI 最慢，放在锁外

        lock (_statsLock)
        {
            _snapshot.CpuPower.Update(cpuPower);
            _snapshot.FanRpm.Update(fanRpm);
            return _snapshot;
        }
    }

    /// <summary>重置最低/最高/平均统计（UI 线程调用，与后台采样互斥）。</summary>
    public void Reset()
    {
        lock (_statsLock)
        {
            _snapshot.CpuPower.Reset();
            _snapshot.FanRpm.Reset();
        }
    }

    /// <summary>依据 CPU 型号后缀估算功耗阈值（TDP 级别）：HX/HK≈55W、H/HS/HQ≈45W、P≈28W、U≈15W，未知取 35W。
    /// 功耗：≤TDP 绿（正常）、TDP~1.6×TDP 橙（偏高，长时间高负载）、&gt;1.6×TDP 红（超高）；
    /// 风扇（笔记本通用量程）：≤3200 绿、3200~4800 橙、&gt;4800 红。</summary>
    public (double PowerElevated, double PowerCritical, double FanElevated, double FanCritical) GetThresholds()
    {
        string name = string.Empty;
        foreach (IHardware hw in _computer.Hardware)
        {
            if (hw.HardwareType == HardwareType.Cpu)
            {
                name = hw.Name?.ToUpperInvariant() ?? string.Empty;
                break;
            }
        }

        double tdp =
            name.EndsWith("HX") || name.EndsWith("HK") ? 55.0 :
            name.EndsWith("HS") || name.EndsWith("HQ") || name.EndsWith("H") ? 45.0 :
            name.EndsWith("P") ? 28.0 :
            name.EndsWith("U") ? 15.0 :
            35.0;

        return (tdp, Math.Round(tdp * 1.6), 3200, 4800);
    }

    /// <summary>风扇转速（RPM），3 秒缓存内的重复读取直接返回上次结果。</summary>
    private float? ReadFanRpmCached()
    {
        if ((DateTime.Now - _lastFanRead).TotalSeconds < FanCacheSeconds)
            return _cachedFanRpm;

        _cachedFanRpm = _ec.ReadFanRpm();
        _lastFanRead = DateTime.Now;
        return _cachedFanRpm;
    }

    /// <summary>首次发现 Package/Total 功耗传感器后缓存，之后直接读 Value（LHM 传感器对象跨更新稳定）。</summary>
    private static ISensor? FindPowerSensor(IHardware hw)
    {
        foreach (ISensor sensor in hw.Sensors)
        {
            if (sensor.SensorType == SensorType.Power &&
                (sensor.Name.Contains("Package", StringComparison.OrdinalIgnoreCase) ||
                 sensor.Name.Contains("Total", StringComparison.OrdinalIgnoreCase)))
            {
                return sensor;
            }
        }

        foreach (IHardware sub in hw.SubHardware)
        {
            ISensor? found = FindPowerSensor(sub);
            if (found is not null)
                return found;
        }

        return null;
    }

    public void Dispose()
    {
        _computer.Close();
        _ec.Dispose();
    }
}
