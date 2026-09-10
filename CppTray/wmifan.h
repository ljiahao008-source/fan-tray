#pragma once
// 机械革命私有 EC 风扇读取：ACPI WMI PowerSwitchInterface（root\WMI）
// 协议对照 MechrevoEcProvider.cs（自有代码）

#include <windows.h>
#include <oleauto.h>   // BSTR
#include <string>

class MechrevoFan {
public:
    MechrevoFan() = default;
    ~MechrevoFan() { Close(); }

    // 连接 root\WMI 并定位 PowerSwitchInterface 实例。失败返回 false（风扇显示 "--"）。
    bool Init();

    // 风扇转速 RPM（优先风扇 1，无效回退风扇 2）。无效返回负值。
    float ReadFanRpm();

    void Close();

private:
    void* _obj = nullptr;   // IWbemClassObject*（实例）
    void* _cls = nullptr;   // IWbemClassObject*（类对象，GetMethod 需在类上）
    void* _wmi = nullptr;   // IWbemServices*
    void* _loc = nullptr;   // IWbemLocator*
    BSTR _path = nullptr;   // 实例相对路径（PowerSwitchInterface.InstanceName="..."）
    bool _comInited = false;
};
