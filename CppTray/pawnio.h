#pragma once
// PawnIO 内核驱动封装：CreateFile + DeviceIoControl
// 协议对照 LibreHardwareMonitor 的 PawnIo.cs（MPL-2.0）

#include <windows.h>
#include <cstdint>

class PawnIo {
public:
    PawnIo() = default;
    ~PawnIo() { Close(); }

    // 从自身资源加载固件模块（resId：RCDATA 资源 ID，如 101=IntelMSR / 103=AMDFamily17）
    bool LoadModuleFromResource(HINSTANCE hInst, WORD resId = 101);

    bool IsLoaded() const { return _h != INVALID_HANDLE_VALUE; }

    // 读取 MSR（低 32 位 EAX / 高 32 位 EDX）。失败返回 false。
    bool ReadMsr(uint32_t index, uint32_t& eax, uint32_t& edx);

    // 读取 AMD SMN 寄存器（System Management Network，如温度 0x59800）。失败返回 false。
    bool ReadSmn(uint32_t offset, uint32_t& value);

    void Close();

private:
    HANDLE _h = INVALID_HANDLE_VALUE;
};
