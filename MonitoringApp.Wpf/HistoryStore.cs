using System;
using System.Collections.Generic;

namespace MonitoringApp;

/// <summary>可切换的指标类型。</summary>
public enum MetricKind
{
    Power,
    Temperature,
    Fan,
    Memory,
    NetworkDown,
    NetworkUp,
}

/// <summary>长历史采样存储。各指标独立存序列，读取失败（null）时跳过该点，避免塞 0 污染曲线。</summary>
public sealed class HistoryStore
{
    private readonly List<(DateTime Time, float Value)> _power = new();
    private readonly List<(DateTime Time, float Value)> _temperature = new();
    private readonly List<(DateTime Time, float Value)> _fan = new();
    private readonly List<(DateTime Time, float Value)> _memory = new();
    private readonly List<(DateTime Time, float Value)> _netDown = new();
    private readonly List<(DateTime Time, float Value)> _netUp = new();
    private readonly TimeSpan _retention;

    public HistoryStore(TimeSpan retention)
    {
        _retention = retention;
    }

    public void Add(float? power, float? temperature, float? fan, float? memory, float? netDownKbps, float? netUpKbps)
    {
        DateTime now = DateTime.Now;
        if (power is { } p) _power.Add((now, p));
        if (temperature is { } t) _temperature.Add((now, t));
        if (fan is { } f) _fan.Add((now, f));
        if (memory is { } m) _memory.Add((now, m));
        if (netDownKbps is { } nd) _netDown.Add((now, nd));
        if (netUpKbps is { } nu) _netUp.Add((now, nu));

        DateTime cutoff = now - _retention;
        Trim(_power, cutoff);
        Trim(_temperature, cutoff);
        Trim(_fan, cutoff);
        Trim(_memory, cutoff);
        Trim(_netDown, cutoff);
        Trim(_netUp, cutoff);
    }

    /// <summary>按周期聚合为均值序列（用于曲线图，每个周期一个点）。</summary>
    public List<(DateTime Time, float Value)> GetSeries(MetricKind kind, TimeSpan period, int maxPoints = 500)
    {
        List<(DateTime Time, float Value)> src = Source(kind);
        List<(DateTime Time, float Value)> result = new();
        if (src.Count == 0)
            return result;

        long pt = period.Ticks;
        long bucket = src[0].Time.Ticks / pt;
        float sum = 0;
        int count = 0;
        DateTime bucketTime = src[0].Time;

        foreach ((DateTime Time, float Value) s in src)
        {
            long b = s.Time.Ticks / pt;
            if (b != bucket)
            {
                result.Add((bucketTime, count > 0 ? sum / count : 0));
                bucket = b;
                sum = 0;
                count = 0;
                bucketTime = s.Time;
            }
            sum += s.Value;
            count++;
        }
        result.Add((bucketTime, count > 0 ? sum / count : 0));

        if (result.Count > maxPoints)
            result.RemoveRange(0, result.Count - maxPoints);

        return result;
    }

    private List<(DateTime Time, float Value)> Source(MetricKind kind) => kind switch
    {
        MetricKind.Power => _power,
        MetricKind.Temperature => _temperature,
        MetricKind.Fan => _fan,
        MetricKind.Memory => _memory,
        MetricKind.NetworkDown => _netDown,
        MetricKind.NetworkUp => _netUp,
        _ => _power,
    };

    private static void Trim(List<(DateTime Time, float Value)> list, DateTime cutoff)
    {
        int remove = 0;
        while (remove < list.Count && list[remove].Time < cutoff)
            remove++;
        if (remove > 0)
            list.RemoveRange(0, remove);
    }

    public void Clear()
    {
        _power.Clear();
        _temperature.Clear();
        _fan.Clear();
        _memory.Clear();
        _netDown.Clear();
        _netUp.Clear();
    }
}
