// 精简监控核心：仅 CPU 功耗（LibreHardwareMonitor）+ 风扇转速（机械革命私有 EC）。

using System;
using System.Text.RegularExpressions;
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
        // LHM 传感器无值时常返回 NaN 而非 null：不过滤会污染 min/max/avg 并把界面刷成 "NaN"
        if (value is not { } v || float.IsNaN(v))
        {
            Current = null;
            return;
        }

        Current = value;
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
/// · 风扇转速：机械革命私有 ACPI WMI（PowerSwitchInterface），每拍直读无值缓存（与控制中心读取方式一致）。
/// 资源优化：CPU 硬件与功耗传感器对象跨更新稳定存在，首次找到后缓存引用，
/// 每秒只做一次硬件 Update + 一次传感器取值，免去每秒枚举硬件树和传感器名字字符串匹配；
/// 统计区加锁，保证后台采样线程与 UI 线程的"重置统计"互不踩踏。
/// </summary>
public sealed class MonitorCore : IDisposable
{
    // 风扇无值缓存：与控制中心一致（其采集线程每 1.5s 直读 WMI，无缓存），我们按 1s 采样间隔每拍直读

    private const int PowerDeadReviveSeconds = 60;   // 功耗持续为 0/空超过该秒数 → 拉起 PawnIO 驱动并重建引擎

    private Computer _computer;
    private readonly MechrevoEcProvider _ec;
    private readonly MonitoringSnapshot _snapshot = new();
    private readonly object _statsLock = new();
    private IHardware? _cachedCpu;
    private ISensor? _cachedPowerSensor;
    private DateTime _powerDeadSince = DateTime.MinValue;
    private float? _lastFanRpm;
    private int _fanFailStreak;

    public MonitorCore()
    {
        _computer = CreateComputer();
        _computer.Open();
        _ec = new MechrevoEcProvider();
    }

    private static Computer CreateComputer() => new()
    {
        IsCpuEnabled = true,
        IsGpuEnabled = false,
        IsMemoryEnabled = false,
        IsMotherboardEnabled = false,
    };

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

        float? fanRpm = _ec.ReadFanRpm();   // WMI 最慢，放在锁外；与控制中心一致：每拍直读，无值缓存

        // 防抖：WMI 偶发一拍失败/返回无效值属正常抖动，沿用上次有效值显示，
        // 连续 3 拍（约 3 秒）都失败才判定真无数据显示 "--"
        if (fanRpm is null)
        {
            if (++_fanFailStreak < 3)
                fanRpm = _lastFanRpm;
            else
                _lastFanRpm = null;
        }
        else
        {
            _fanFailStreak = 0;
            _lastFanRpm = fanRpm;
        }

        // 功耗自愈：持续为 0/null 说明 PawnIO 内核驱动没起来（开机按需启动竞态/被优化软件禁用），
        // 拉起驱动 + 重建引擎。已提权进程执行 sc start 无副作用；驱动正常时此分支永远不触发。
        if (cpuPower is null or 0)
        {
            if (_powerDeadSince == DateTime.MinValue)
            {
                _powerDeadSince = DateTime.Now;
            }
            else if ((DateTime.Now - _powerDeadSince).TotalSeconds >= PowerDeadReviveSeconds)
            {
                ReviveEngine();
                _powerDeadSince = DateTime.Now;
            }
        }
        else
        {
            _powerDeadSince = DateTime.MinValue;
        }

        lock (_statsLock)
        {
            _snapshot.CpuPower.Update(cpuPower);
            _snapshot.FanRpm.Update(fanRpm);
            return _snapshot;
        }
    }

    /// <summary>自愈：拉起 PawnIO 驱动服务并重建 LHM 引擎（采样后台线程调用，风扇的 _ec 与引擎无关不受影响）。</summary>
    private void ReviveEngine()
    {
        try
        {
            using System.Diagnostics.Process? p = System.Diagnostics.Process.Start(
                new System.Diagnostics.ProcessStartInfo("sc.exe", "start PawnIO")
                {
                    CreateNoWindow = true,
                    UseShellExecute = false,
                });
            p?.WaitForExit(5000);
        }
        catch
        {
            // 拉起失败不打断采样，下个周期再试
        }

        try
        {
            _cachedCpu = null;
            _cachedPowerSensor = null;
            _computer.Close();
            _computer = CreateComputer();
            _computer.Open();
        }
        catch
        {
            // 重建失败同样留到下个周期重试
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

        // 从型号数字后提取后缀（如 8745H / 7945HX / 1360P）：LHM 的 CPU 名称常带
        // "W/ RADEON 780M GRAPHICS" 之类的尾巴，直接 EndsWith 会永远落到默认值
        double tdp = 35.0;
        Match m = Regex.Match(name, @"(\d{2,5})(HX|HK|HS|HQ|H|U|P)\b");
        if (m.Success)
        {
            tdp = m.Groups[2].Value switch
            {
                "HX" or "HK" => 55.0,
                "HS" or "HQ" or "H" => 45.0,
                "P" => 28.0,
                "U" => 15.0,
                _ => 35.0,
            };
        }

        return (tdp, Math.Round(tdp * 1.6), 3200, 4800);
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
