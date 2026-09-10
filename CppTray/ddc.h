#pragma once
// ─────────────────────────────────────────────────────────────
// ddc.h —— 完整 DDC/CI 模块（Twinkle Tray node-ddcci 全功能移植）
// 能力：显示器枚举 + 能力字符串解析（vcp 列表与定义值）+ 任意 VCP 读写
//       + 亮度/对比度高低层读写 + 音量/输入源/电源
// 无 UI、无自有消息循环；依赖 dxva2.lib + user32.lib。
// ─────────────────────────────────────────────────────────────
#include <windows.h>
#include <string>
#include <vector>

namespace ddc {

// VCP 代码（MCCS 标准）
constexpr BYTE kVcpLuminance = 0x10;   // 亮度
constexpr BYTE kVcpContrast = 0x12;    // 对比度
constexpr BYTE kVcpInputSource = 0x60; // 输入源
constexpr BYTE kVcpAudioVolume = 0x62; // 音量
constexpr BYTE kVcpPower = 0xD6;       // 电源模式

// 单个 VCP 特性（从能力字符串解析）
struct VcpFeature {
    BYTE code = 0;
    std::vector<DWORD> values;   // 定义值（枚举类）；空 = 连续调节
    bool HasValue(DWORD v) const {
        for (auto x : values)
            if (x == v) return true;
        return false;
    }
};

// 显示器快照（Enumerate 返回）
struct Monitor {
    std::wstring deviceName;          // "\\.\DISPLAY1"
    std::wstring description;         // EDID 名称
    bool ddcSupported = false;        // 能力字符串/高低层读取成功
    std::vector<VcpFeature> features; // 解析出的全部 VCP 特性

    bool HasCode(BYTE code) const {
        for (auto& f : features)
            if (f.code == code) return true;
        return false;
    }
    // 连续调节码的定义值（0x60 输入源等）
    std::vector<DWORD> DefinedValues(BYTE code) const {
        for (auto& f : features)
            if (f.code == code) return f.values;
        return {};
    }

    // 高低层亮度范围（GetMonitorBrightness）
    bool hasBrightness = false;
    DWORD brightnessMin = 0, brightnessCur = 0, brightnessMax = 0;
    // 高低层对比度范围（GetMonitorContrast）
    bool hasContrast = false;
    DWORD contrastMin = 0, contrastCur = 0, contrastMax = 0;
    // 音量（0x62）
    bool hasVolume = false;
    DWORD volumeCur = 0, volumeMax = 100;
    // 输入源
    std::vector<DWORD> inputs;        // 0x60 定义值
    DWORD inputCur = 0;
    bool hasInputs = false;
    // 电源（0xD6 存在即可控制）
    bool hasPower = false;
};

class Ddc {
public:
    // 枚举全部显示器并解析能力（每次重新枚举，天然支持热插拔）
    static std::vector<Monitor> Enumerate();

    // 任意 VCP 读取/写入（deviceName 为空 = 主显示器）。写入失败自动重建句柄缓存。
    static bool GetVCP(const std::wstring& deviceName, BYTE code, DWORD& cur, DWORD& max);
    static bool SetVCP(const std::wstring& deviceName, BYTE code, DWORD value);

    // 亮度/对比度百分比设定（0-100）
    static bool SetBrightnessPercent(const std::wstring& deviceName, int percent);
    static bool SetContrastPercent(const std::wstring& deviceName, int percent);

    // 批量相对调整：target=0 亮度 / 1 对比度 / 2 音量，step 为百分比，返回成功台数
    static int AdjustAll(int target, int stepPercent);

    // 释放内部缓存句柄（宿主退出/设备变更时调用）
    static void InvalidateCache();
};

}  // namespace ddc
