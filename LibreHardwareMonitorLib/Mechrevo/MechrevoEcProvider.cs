// This file is NOT derived from MPL-2.0 code. It is an original addition.
// 机械革命私有 EC 接口读取器（独立新增，非 MPL 衍生，可自选许可）

#nullable enable

using System;
using System.Management;

namespace LibreHardwareMonitor.Mechrevo;

/// <summary>
/// 通过 ACPI WMI <c>PowerSwitchInterface</c>（ACPI\PNP0C14\IP3POWERSWITCH_0）读取机械革命私有 EC 数据。
/// LibreHardwareMonitor 原生无法读取机械革命机型的风扇转速（私有协议，不走标准 SMBus/Super I/O），
/// 本 Provider 通过 <c>GetFanControl</c> 补齐。
/// </summary>
public sealed class MechrevoEcProvider : IDisposable
{
    private const uint Invalid = 2147483647; // 0x7FFFFFFF，EC 返回的"无效/不支持"标记

    private readonly ManagementObject? _ps;

    public MechrevoEcProvider()
    {
        try
        {
            ManagementScope scope = new(@"root\WMI");
            scope.Connect();
            ManagementObjectSearcher searcher = new(scope, new ObjectQuery("SELECT * FROM PowerSwitchInterface"));
            foreach (ManagementObject obj in searcher.Get())
            {
                _ps = obj;
                break;
            }
        }
        catch
        {
            // 机型不支持 / 权限不足 / 类不存在时，保持 _ps = null，
            // 风扇显示 "--"，但不影响其他监控指标，也不导致程序启动崩溃。
            _ps = null;
        }
    }

    /// <summary>风扇转速（RPM）。与控制中心 GetFanRPM 同构：FanDuty 低 16 位为风扇 1，高 16 位为风扇 2。
    /// 优先取风扇 1（与控制中心显示逻辑一致），风扇 1 无效时回退风扇 2（部分机型 CPU 风扇接在通道 2）。</summary>
    public float? ReadFanRpm()
    {
        (float? fan1, float? fan2) = ReadFanRpms();
        if (fan1 is { } v1)
            return v1;
        return fan2;
    }

    /// <summary>同时读取双风扇：FanDuty 低 16 位为风扇 1、高 16 位为风扇 2（0 或 0x7FFFFFFF 视为无效）。</summary>
    public (float? Fan1, float? Fan2) ReadFanRpms()
    {
        if (_ps == null)
            return (null, null);

        try
        {
            ManagementBaseObject inParams = _ps.GetMethodParameters("GetFanControl");
            inParams["FanNumber"] = (byte)1;
            ManagementBaseObject outParams = _ps.InvokeMethod("GetFanControl", inParams, null);

            if (outParams?["FanDuty"] is not null)
            {
                uint fanDuty = Convert.ToUInt32(outParams["FanDuty"]);
                return (ParseRpm(fanDuty & 0xFFFF), ParseRpm((fanDuty >> 16) & 0xFFFF));
            }
        }
        catch
        {
            // WMI 读取失败：返回 null 降级，不向上抛异常拖垮其他指标。
        }

        return (null, null);
    }

    private static float? ParseRpm(uint raw) => raw is > 0 and < Invalid ? raw : null;

    public void Dispose() => _ps?.Dispose();
}
