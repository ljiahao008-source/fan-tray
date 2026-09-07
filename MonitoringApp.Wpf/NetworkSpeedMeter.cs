using System;
using System.Net.NetworkInformation;

namespace MonitoringApp;

/// <summary>网络速率采样器：每 tick 对系统网络接口收发字节计数做差分，得到实时上行/下行速率（KB/s）。</summary>
public sealed class NetworkSpeedMeter
{
    private long _prevIn, _prevOut;
    private DateTime _prevTime;

    /// <summary>下行速率（KB/s）。</summary>
    public double DownKbps { get; private set; }

    /// <summary>上行速率（KB/s）。</summary>
    public double UpKbps { get; private set; }

    /// <summary>本次会话累计下行（MB）。</summary>
    public double TotalDownMB { get; private set; }

    /// <summary>本次会话累计上行（MB）。</summary>
    public double TotalUpMB { get; private set; }

    /// <summary>虚拟网卡关键词：代理 TUN/TAP、虚拟机、隧道等会把同一份流量重复计数，全部排除。</summary>
    private static readonly string[] VirtualKeywords =
    {
        "tun", "tap", "clash", "mihomo", "sing-box", "singbox", "wireguard", "openvpn",
        "vethernet", "hyper-v", "vmware", "virtualbox", "loopback", "vpn", "wintun",
        "tailscale", "zerotier", "teredo", "isatap", "wan miniport",
    };

    /// <summary>是否虚拟/隧道接口（按类型 + 描述/名称关键词双重判断）。</summary>
    private static bool IsVirtual(NetworkInterface ni)
    {
        if (ni.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel)
            return true;
        string d = ni.Description.ToLowerInvariant();
        string n = ni.Name.ToLowerInvariant();
        foreach (string k in VirtualKeywords)
        {
            if (d.Contains(k) || n.Contains(k))
                return true;
        }
        return false;
    }

    /// <summary>执行一次采样并更新速率/累计。建议 1 秒调用一次。</summary>
    public void Sample()
    {
        long inBytes = 0, outBytes = 0;
        try
        {
            foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                // 只统计"已连接"的真实接口：跳过回环/隧道/虚拟网卡（代理 TUN、虚拟机等会把同一份流量重复计数）
                if (ni.OperationalStatus != OperationalStatus.Up || IsVirtual(ni))
                    continue;

                // IP 层统计（IPv4+IPv6 合计），比仅统计 IPv4 更完整
                IPInterfaceStatistics st = ni.GetIPStatistics();
                inBytes += st.BytesReceived;
                outBytes += st.BytesSent;
            }
        }
        catch
        {
            // 采样失败保持上次值，不中断监控
        }

        if (_prevTime != default)
        {
            double dt = (DateTime.UtcNow - _prevTime).TotalSeconds;
            if (dt > 0.05)
            {
                DownKbps = Math.Max(0, (inBytes - _prevIn) / 1024.0 / dt);
                UpKbps = Math.Max(0, (outBytes - _prevOut) / 1024.0 / dt);
                if (inBytes >= _prevIn) TotalDownMB += DownKbps * dt / 1024.0;
                if (outBytes >= _prevOut) TotalUpMB += UpKbps * dt / 1024.0;
            }
        }

        _prevIn = inBytes;
        _prevOut = outBytes;
        _prevTime = DateTime.UtcNow;
    }

    /// <summary>把 KB/s 拆成"数值 + 单位"，便于界面自动切 KB/MB。</summary>
    public static (double Num, string Unit) Format(double kbps)
    {
        if (kbps >= 1024)
            return (kbps / 1024.0, "MB/s");
        return (kbps, "KB/s");
    }

    /// <summary>把 KB/s 格式化为可读文本（如 "1.2 MB/s" / "350 KB/s"）。</summary>
    public static string FormatText(double kbps)
    {
        (double num, string unit) = Format(kbps);
        return (unit == "MB/s" ? num.ToString("0.0") : num.ToString("0")) + " " + unit;
    }
}
