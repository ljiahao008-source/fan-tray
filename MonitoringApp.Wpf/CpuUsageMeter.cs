using System;
using System.Diagnostics;

namespace MonitoringApp;

/// <summary>整机 CPU 占用率采样器（PerformanceCounter "% Processor Time" · "_Total"）。
/// 构造时预热一次计数器，之后每次 <see cref="Next"/> 返回距上次调用期间的平均占用率（0~100）。</summary>
public sealed class CpuUsageMeter : IDisposable
{
    private readonly PerformanceCounter _counter = new("Processor", "% Processor Time", "_Total", readOnly: true);

    public CpuUsageMeter()
    {
        // 首次 NextValue 恒为 0，先预热掉
        _ = _counter.NextValue();
    }

    public void Dispose() => _counter.Dispose();

    /// <summary>采样一次，返回 0~100 的占用率。</summary>
    public float Next() => Math.Clamp(_counter.NextValue(), 0f, 100f);
}
