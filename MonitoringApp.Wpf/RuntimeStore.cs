using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace MonitoringApp;

/// <summary>运行时长记录：累计总时长 + 按日时长（保留 90 天），持久化到 exe 旁 runtime.json。</summary>
public sealed class RuntimeStore
{
    private static string FilePath => Path.Combine(AppContext.BaseDirectory, "runtime.json");
    private const int KeepDays = 90;

    /// <summary>累计运行分钟数。</summary>
    public double TotalMinutes { get; set; }

    /// <summary>按日运行分钟数（key = yyyy-MM-dd）。</summary>
    public Dictionary<string, double> DailyMinutes { get; set; } = new();

    private static string DayKey(DateTime t) => t.ToString("yyyy-MM-dd");

    public double GetMinutes(DateTime day) =>
        DailyMinutes.TryGetValue(DayKey(day), out double v) ? v : 0;

    /// <summary>累加一段运行时长（自动归日、裁剪过期记录）。</summary>
    public void Add(DateTime now, double minutes)
    {
        if (minutes <= 0)
            return;
        TotalMinutes += minutes;
        string key = DayKey(now);
        DailyMinutes[key] = DailyMinutes.TryGetValue(key, out double v) ? v + minutes : minutes;
        Prune(now);
    }

    private void Prune(DateTime now)
    {
        if (DailyMinutes.Count <= KeepDays)
            return;
        DateTime cutoff = now.AddDays(-KeepDays);
        var expired = new List<string>();
        foreach (var (key, _) in DailyMinutes)
        {
            if (DateTime.TryParse(key, out DateTime day) && day < cutoff)
                expired.Add(key);
        }
        foreach (string key in expired)
            DailyMinutes.Remove(key);
    }

    public static RuntimeStore Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                string raw = File.ReadAllText(FilePath);
                RuntimeStore? s = JsonSerializer.Deserialize<RuntimeStore>(raw);
                if (s != null)
                    return s;
            }
        }
        catch
        {
            // 损坏时回退新档案
        }
        return new RuntimeStore();
    }

    public void Save()
    {
        try
        {
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // 忽略写入失败
        }
    }
}
