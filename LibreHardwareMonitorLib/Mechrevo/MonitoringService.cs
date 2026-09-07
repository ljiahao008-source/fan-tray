// This file is NOT derived from MPL-2.0 code. It is an original addition.
// 精简监控服务（独立新增，非 MPL 衍生，可自选许可）

using System;
using System.Globalization;
using LibreHardwareMonitor.Hardware;
using Windows.Win32;
using Windows.Win32.System.SystemInformation;

namespace LibreHardwareMonitor.Mechrevo;

/// <summary>单指标统计：当前值 + 最低 / 最高 / 平均。</summary>
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

/// <summary>四指标监控快照：CPU 功耗、CPU 温度、风扇转速、内存占用（各含最低/最高/平均）。</summary>
public sealed class MonitoringSnapshot
{
    public Metric CpuPower { get; } = new();
    public Metric CpuTemperature { get; } = new();
    public Metric FanRpm { get; } = new();
    public Metric MemoryLoad { get; } = new();

    public string ToChineseString() =>
        $"CPU 功耗 {Fmt(CpuPower)} W | CPU 温度 {Fmt(CpuTemperature)}°C | 风扇转速 {Fmt(FanRpm)} RPM | 内存占用 {Fmt(MemoryLoad)}%";

    private static string Fmt(Metric m) => m.Current?.ToString("0", CultureInfo.InvariantCulture) ?? "--";
}

/// <summary>
/// 精简监控服务：只暴露目标四指标，替代 LHM 的裸 <c>Hardware[]</c> 树。
/// </summary>
public sealed class MonitoringService : IDisposable
{
    private readonly Computer _computer;
    private readonly MechrevoEcProvider _ec;
    private readonly MonitoringSnapshot _snapshot = new();

    public MonitoringService()
    {
        _computer = new Computer
        {
            IsCpuEnabled = true,
            IsGpuEnabled = false,
            IsMemoryEnabled = false, // 内存占用直接走 GlobalMemoryStatusEx，避开 LHM 物理/虚拟内存同名传感器
            IsMotherboardEnabled = false,
        };
        _computer.Open();
        _ec = new MechrevoEcProvider();
    }

    /// <summary>单次读取并更新四指标快照。</summary>
    public MonitoringSnapshot Read()
    {
        float? cpuTemp = null, cpuPower = null;

        foreach (IHardware hw in _computer.Hardware)
        {
            hw.Update();
            Collect(hw, ref cpuTemp, ref cpuPower);
        }

        _snapshot.CpuPower.Update(cpuPower);
        _snapshot.CpuTemperature.Update(cpuTemp);
        _snapshot.FanRpm.Update(_ec.ReadFanRpm());
        _snapshot.MemoryLoad.Update(ReadPhysicalMemoryLoad());

        return _snapshot;
    }

    /// <summary>物理内存占用（%）。与原项目 MemoryWindows.cs 同算法（GlobalMemoryStatusEx）。</summary>
    private static unsafe float? ReadPhysicalMemoryLoad()
    {
        MEMORYSTATUSEX status = new() { dwLength = (uint)sizeof(MEMORYSTATUSEX) };
        if (!PInvoke.GlobalMemoryStatusEx(ref status) || status.ullTotalPhys == 0)
            return null;

        return (float)((status.ullTotalPhys - status.ullAvailPhys) * 100.0 / status.ullTotalPhys);
    }

    /// <summary>重置所有指标的最低/最高/平均统计。</summary>
    public void Reset()
    {
        _snapshot.CpuPower.Reset();
        _snapshot.CpuTemperature.Reset();
        _snapshot.FanRpm.Reset();
        _snapshot.MemoryLoad.Reset();
    }

    private static void Collect(IHardware hw, ref float? cpuTemp, ref float? cpuPower)
    {
        foreach (ISensor sensor in hw.Sensors)
        {
            if (sensor.Value is not { } v)
                continue;

            switch (hw.HardwareType)
            {
                case HardwareType.Cpu when sensor.SensorType == SensorType.Temperature:
                    if (sensor.Name.Contains("Tctl", StringComparison.OrdinalIgnoreCase) ||
                        sensor.Name.Contains("Tdie", StringComparison.OrdinalIgnoreCase) ||
                        sensor.Name.Contains("Package", StringComparison.OrdinalIgnoreCase))
                        cpuTemp ??= v;
                    break;

                case HardwareType.Cpu when sensor.SensorType == SensorType.Power:
                    if (sensor.Name.Contains("Package", StringComparison.OrdinalIgnoreCase) ||
                        sensor.Name.Contains("Total", StringComparison.OrdinalIgnoreCase))
                        cpuPower ??= v;
                    break;
            }
        }

        foreach (IHardware sub in hw.SubHardware)
            Collect(sub, ref cpuTemp, ref cpuPower);
    }

    public void Dispose()
    {
        _computer.Close();
        _ec.Dispose();
    }
}
