using System;
using System.Collections.Generic;

namespace MonitoringApp;

/// <summary>单个趋势采样点。</summary>
public readonly struct TrendPoint
{
    public TrendPoint(DateTime time, float power, float temperature)
    {
        Time = time;
        Power = power;
        Temperature = temperature;
    }

    public DateTime Time { get; }
    public float Power { get; }
    public float Temperature { get; }
}

/// <summary>实时趋势环形缓冲（用于曲线图）。</summary>
public sealed class TrendHistory
{
    private readonly Queue<TrendPoint> _points = new();
    private readonly int _maxPoints;

    public TrendHistory(int maxPoints = 600)
    {
        _maxPoints = maxPoints;
    }

    public void Add(float power, float temperature)
    {
        _points.Enqueue(new TrendPoint(DateTime.Now, power, temperature));
        while (_points.Count > _maxPoints)
            _points.Dequeue();
    }

    public IReadOnlyCollection<TrendPoint> Points => _points;

    public void Clear() => _points.Clear();
}
