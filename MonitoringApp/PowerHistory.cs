using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace MonitoringApp;

/// <summary>单日监控记录（四个指标各保存采样列表）。
/// <see cref="Samples"/> 字段名保留以兼容旧版仅功耗的 powerlog.json。</summary>
public sealed class DailyPower
{
    /// <summary>CPU 功耗采样（W）。字段名兼容旧版数据，实为功耗。</summary>
    public List<float> Samples { get; set; } = new();

    /// <summary>CPU 温度采样（℃）。</summary>
    public List<float> Temperature { get; set; } = new();

    /// <summary>风扇转速采样（RPM）。</summary>
    public List<float> Fan { get; set; } = new();

    /// <summary>内存占用采样（%）。</summary>
    public List<float> Memory { get; set; } = new();

    /// <summary>功耗采样数（主口径）。</summary>
    public int SampleCount => Samples.Count;

    public float Average => Samples.Count > 0 ? Samples.Average() : 0;
    public float Max => Samples.Count > 0 ? Samples.Max() : 0;
    public float Min => Samples.Count > 0 ? Samples.Min() : 0;

    /// <summary>某指标列表的汇总（空列表返回三个 null）。</summary>
    public static (float? Avg, float? Max, float? Min) Summary(List<float> list) =>
        list.Count == 0
            ? (null, null, null)
            : (list.Average(), list.Max(), list.Min());
}

/// <summary>每日监控历史（四指标采样 + 持久化到 exe 旁 powerlog.json）。</summary>
public sealed class PowerHistory
{
    private static readonly string LogPath = Path.Combine(AppContext.BaseDirectory, "powerlog.json");
    private readonly Dictionary<string, DailyPower> _days = new();

    /// <summary>记录一次四指标采样（某一项为 null 则该项跳过本次，不塞 0）。</summary>
    public void AddSample(float? power, float? temperature, float? fan, float? memory)
    {
        string key = DateTime.Now.ToString("yyyy-MM-dd");
        if (!_days.TryGetValue(key, out DailyPower? d))
        {
            d = new DailyPower();
            _days[key] = d;
        }

        if (power is { } p) d.Samples.Add(p);
        if (temperature is { } t) d.Temperature.Add(t);
        if (fan is { } f) d.Fan.Add(f);
        if (memory is { } m) d.Memory.Add(m);
    }

    /// <summary>获取指定日期的记录。</summary>
    public DailyPower? GetDay(DateTime day)
    {
        return _days.TryGetValue(day.ToString("yyyy-MM-dd"), out DailyPower? d) ? d : null;
    }

    public IReadOnlyDictionary<string, DailyPower> Days => _days;

    public void Save()
    {
        try
        {
            File.WriteAllText(LogPath, JsonSerializer.Serialize(_days));
        }
        catch
        {
            // 忽略写入失败
        }
    }

    public static PowerHistory Load()
    {
        PowerHistory p = new();
        try
        {
            if (File.Exists(LogPath))
            {
                Dictionary<string, DailyPower>? d = JsonSerializer.Deserialize<Dictionary<string, DailyPower>>(File.ReadAllText(LogPath));
                if (d != null)
                {
                    foreach (KeyValuePair<string, DailyPower> kv in d)
                        p._days[kv.Key] = kv.Value;
                }
            }
        }
        catch
        {
            // 忽略读取失败
        }

        return p;
    }
}
