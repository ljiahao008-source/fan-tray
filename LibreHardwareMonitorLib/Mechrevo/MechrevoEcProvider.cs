// This file is NOT derived from MPL-2.0 code. It is an original addition.
// 机械革命私有 EC 接口读取器（独立新增，非 MPL 衍生，可自选许可）

using System;
using System.Management;

namespace LibreHardwareMonitor.Mechrevo;

/// <summary>
/// 通过 ACPI WMI <c>PowerSwitchInterface</c>（ACPI\PNP0C14\IP3POWERSWITCH_0）读取机械革命私有 EC 数据。
/// LibreHardwareMonitor 原生无法读取机械革命机型的风扇转速（私有协议，不走标准 SMBus/Super I/O），
/// 本 Provider 通过 <c>GetFanControl</c> / <c>GetHwTemp</c> 补齐。
/// </summary>
public sealed class MechrevoEcProvider : IDisposable
{
    private const uint Invalid = 2147483647; // 0x7FFFFFFF，EC 返回的"无效/不支持"标记

    private readonly ManagementObject _ps;

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

    /// <summary>是否检测到机械革命 EC 接口。</summary>
    public bool IsAvailable => _ps != null;

    /// <summary>风扇转速（RPM）。返回值低 16 位为风扇 1 转速。</summary>
    public float? ReadFanRpm()
    {
        if (_ps == null)
            return null;

        try
        {
            ManagementBaseObject inParams = _ps.GetMethodParameters("GetFanControl");
            inParams["FanNumber"] = (byte)1;
            ManagementBaseObject outParams = _ps.InvokeMethod("GetFanControl", inParams, null);

            if (outParams?["FanDuty"] is not null)
            {
                uint fanDuty = Convert.ToUInt32(outParams["FanDuty"]);
                uint rpm = fanDuty & 0xFFFF;
                return rpm is > 0 and < Invalid ? rpm : null;
            }
        }
        catch
        {
            // WMI 读取失败：返回 null 降级，不向上抛异常拖垮其他指标。
        }

        return null;
    }

    /// <summary>硬件温度（°C）。<paramref name="hwTempType"/> = 1 为 CPU 温度。</summary>
    public float? ReadTemperature(byte hwTempType = 1)
    {
        if (_ps == null)
            return null;

        try
        {
            ManagementBaseObject inParams = _ps.GetMethodParameters("GetHwTemp");
            inParams["HwTempType"] = hwTempType;
            ManagementBaseObject outParams = _ps.InvokeMethod("GetHwTemp", inParams, null);

            if (outParams?["Temp"] is not null)
            {
                uint temp = Convert.ToUInt32(outParams["Temp"]);
                return temp < Invalid ? temp : null;
            }
        }
        catch
        {
            // WMI 读取失败：返回 null 降级。
        }

        return null;
    }

    public void Dispose() => _ps?.Dispose();
}
