// ddc.cpp —— 完整 DDC/CI 模块实现（Twinkle Tray node-ddcci 移植）
// 能力解析、任意 VCP 读写、亮度/对比度高低层、音量/输入源/电源；
// 瞬时 DDC 错误重试、句柄失效重建、静默降级。

#include "ddc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <thread>

// Windows SDK 26100 起不再随带 dxva2.h，按 MSDN 自声明最小接口并链 dxva2.lib
#ifndef PHYSICAL_MONITOR_DESCRIPTION_SIZE
#define PHYSICAL_MONITOR_DESCRIPTION_SIZE 128
#endif
typedef struct _PHYSICAL_MONITOR {
    HANDLE hPhysicalMonitor;
    WCHAR  szPhysicalMonitorDescription[PHYSICAL_MONITOR_DESCRIPTION_SIZE];
} PHYSICAL_MONITOR;
extern "C" {
BOOL WINAPI GetNumberOfPhysicalMonitorsFromHMONITOR(HMONITOR hMonitor, LPDWORD pdwNumberOfPhysicalMonitors);
BOOL WINAPI GetPhysicalMonitorsFromHMONITOR(HMONITOR hMonitor, DWORD dwPhysicalMonitorArraySize, PHYSICAL_MONITOR* pPhysicalMonitorArray);
BOOL WINAPI DestroyPhysicalMonitor(HANDLE hMonitor);
BOOL WINAPI GetMonitorBrightness(HANDLE hMonitor, LPDWORD pdwMinimumBrightness, LPDWORD pdwCurrentBrightness, LPDWORD pdwMaximumBrightness);
BOOL WINAPI GetMonitorContrast(HANDLE hMonitor, LPDWORD pdwMinimumContrast, LPDWORD pdwCurrentContrast, LPDWORD pdwMaximumContrast);
BOOL WINAPI SetVCPFeature(HANDLE hMonitor, BYTE bVCPCode, DWORD dwNewValue);
BOOL WINAPI GetVCPFeatureAndVCPFeatureReply(HANDLE hMonitor, BYTE bVCPCode, LPDWORD pvct, LPDWORD pdwCurrentValue, LPDWORD pdwMaximumValue);
BOOL WINAPI GetCapabilitiesStringLength(HANDLE hMonitor, LPDWORD pdwCapabilitiesStringLengthInCharacters);
BOOL WINAPI CapabilitiesRequestAndCapabilitiesReply(HANDLE hMonitor, LPWSTR pszCapabilitiesString, DWORD dwCapabilitiesStringLengthInCharacters);
BOOL WINAPI SaveCurrentSettings(HANDLE hMonitor);
}

#pragma comment(lib, "dxva2.lib")

namespace ddc {

namespace {

constexpr int   kRetryAttempts = 3;
constexpr auto  kRetryDelay    = std::chrono::milliseconds(50);

struct CachedMonitor {
    std::wstring deviceName;
    std::wstring description;
    HANDLE hPhysical = nullptr;
    bool capsLoaded = false;
    std::vector<VcpFeature> features;
    bool hasBrightness = false;
    DWORD bMin = 0, bCur = 0, bMax = 0;
    bool hasContrast = false;
    DWORD cMin = 0, cCur = 0, cMax = 0;
};

std::vector<CachedMonitor> g_cache;   // 单调用线程模型，模块不加锁

bool IsTransientDdcError(DWORD code) {
    switch (code) {
        case ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA:
        case ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA:
        case ERROR_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE:
        case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_COMMAND:
        case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_LENGTH:
        case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM:
            return true;
        default:
            return false;
    }
}

template <typename F>
bool TryDdc(F op, DWORD& errorCode) {
    for (int attempt = 1; attempt <= kRetryAttempts; attempt++) {
        if (attempt > 1)
            std::this_thread::sleep_for(kRetryDelay);
        if (op())
            return true;
        errorCode = GetLastError();
        if (!IsTransientDdcError(errorCode))
            return false;
    }
    return false;
}

std::wstring DeviceNameOf(HMONITOR h) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, &mi))
        return mi.szDevice;
    return L"";
}

// 内部缓存条目专用查询（公开 Monitor 的成员方法不适用 CachedMonitor）
bool HasCode(const CachedMonitor& m, BYTE code) {
    for (auto& f : m.features)
        if (f.code == code) return true;
    return false;
}
std::vector<DWORD> DefinedValues(const CachedMonitor& m, BYTE code) {
    for (auto& f : m.features)
        if (f.code == code) return f.values;
    return {};
}

// 能力字符串 → VCP 特性表（Twinkle Tray parseCapabilitiesString 移植）
std::vector<VcpFeature> ParseCapabilities(const wchar_t* text) {
    std::vector<VcpFeature> out;
    std::wstring s = text ? text : L"";

    // 找 "vcp("（大小写不敏感、允许空白）
    size_t start = std::wstring::npos;
    for (size_t i = 0; i + 3 < s.size(); i++) {
        if ((s[i] == L'v' || s[i] == L'V') && (s[i + 1] == L'c' || s[i + 1] == L'C') &&
            (s[i + 2] == L'p' || s[i + 2] == L'P')) {
            size_t j = i + 3;
            while (j < s.size() && (s[j] == L' ' || s[j] == L'\t'))
                j++;
            if (j < s.size() && s[j] == L'(') {
                start = j + 1;
                break;
            }
        }
    }
    if (start == std::wstring::npos)
        return out;

    // 括号配平提取内容
    int layers = 1;
    std::wstring body;
    size_t pos = start;
    while (layers > 0 && pos < s.size()) {
        wchar_t c = s[pos];
        if (c == L'(') layers++;
        else if (c == L')') {
            layers--;
            if (layers <= 0) break;
        }
        if (layers > 0 && c != L'\0' && c != L' ')
            body += c;
        pos++;
    }

    // 交替解析 VCP 码（2 位十六进制）与定义值
    size_t p = 0;
    auto hexVal = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    while (p + 1 < body.size()) {
        int hi = hexVal(body[p]), lo = hexVal(body[p + 1]);
        if (hi < 0 || lo < 0)
            break;
        VcpFeature f;
        f.code = (BYTE)((hi << 4) | lo);
        p += 2;
        if (p < body.size() && body[p] == L'(') {
            p++;
            int depth = 0;
            while (p < body.size() && !(body[p] == L')' && depth == 0)) {
                if (body[p] == L'(') {   // 子数据直接跳过
                    depth++;
                    while (depth > 0 && p < body.size()) {
                        if (body[p] == L'(') depth++;
                        else if (body[p] == L')') depth--;
                        p++;
                    }
                } else {
                    int vHi = hexVal(body[p]), vLo = p + 1 < body.size() ? hexVal(body[p + 1]) : -1;
                    if (vHi >= 0 && vLo >= 0) {
                        f.values.push_back((DWORD)((vHi << 4) | vLo));
                        p += 2;
                    } else {
                        p++;
                    }
                }
            }
            p++;   // 跳过 ')'
        }
        out.push_back(f);
    }
    return out;
}

// 枚举 + 能力 + 高低层读取（仅保留 ddcSupported 的显示器）
std::vector<CachedMonitor> EnumerateAll() {
    std::vector<CachedMonitor> out;
    std::vector<HMONITOR> hmons;
    auto cb = [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(h);
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(&hmons));

    for (HMONITOR hm : hmons) {
        DWORD n = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &n) || n == 0)
            continue;
        std::vector<PHYSICAL_MONITOR> pms(n);
        if (!GetPhysicalMonitorsFromHMONITOR(hm, n, pms.data()))
            continue;
        std::wstring dev = DeviceNameOf(hm);
        for (DWORD i = 0; i < n; i++) {
            CachedMonitor cm;
            cm.deviceName  = dev;
            cm.description = pms[i].szPhysicalMonitorDescription;
            cm.hPhysical   = pms[i].hPhysicalMonitor;

            // 能力字符串（不可靠，重试几次；失败不致命）
            DWORD len = 0, err = ERROR_SUCCESS;
            for (int a = 0; a < 3 && len == 0; a++) {
                if (a > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                TryDdc([&] { return GetCapabilitiesStringLength(cm.hPhysical, &len); }, err);
            }
            if (len > 1) {
                std::vector<wchar_t> buf(len + 2, 0);
                BOOL ok = FALSE;
                for (int a = 0; a < 5 && !ok; a++) {
                    if (a > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    ok = CapabilitiesRequestAndCapabilitiesReply(cm.hPhysical, buf.data(), len);
                }
                if (ok)
                    cm.features = ParseCapabilities(buf.data());
                cm.capsLoaded = true;
            }

            // 高低层亮度/对比度
            DWORD minv = 0, curv = 0, maxv = 0;
            if (TryDdc([&] { return GetMonitorBrightness(cm.hPhysical, &minv, &curv, &maxv); }, err) && maxv > minv) {
                cm.hasBrightness = true;
                cm.bMin = minv; cm.bCur = curv; cm.bMax = maxv;
            }
            if (TryDdc([&] { return GetMonitorContrast(cm.hPhysical, &minv, &curv, &maxv); }, err) && maxv > minv) {
                cm.hasContrast = true;
                cm.cMin = minv; cm.cCur = curv; cm.cMax = maxv;
            }

            // DDC 支持判定：能力表或亮度/对比度任一成功
            bool ok = cm.capsLoaded || cm.hasBrightness || cm.hasContrast;
            if (ok) {
                out.push_back(std::move(cm));
            } else {
                DestroyPhysicalMonitor(pms[i].hPhysicalMonitor);
            }
        }
    }
    return out;
}

void DestroyAll(std::vector<CachedMonitor>& ms) {
    for (auto& m : ms) {
        if (m.hPhysical)
            DestroyPhysicalMonitor(m.hPhysical);
        m.hPhysical = nullptr;
    }
    ms.clear();
}

bool EnsureCache() {
    if (!g_cache.empty())
        return true;
    g_cache = EnumerateAll();
    return !g_cache.empty();
}

CachedMonitor* Find(const std::wstring& deviceName) {
    if (g_cache.empty())
        return nullptr;
    if (deviceName.empty())
        return &g_cache.front();
    for (auto& m : g_cache)
        if (m.deviceName == deviceName)
            return &m;
    return nullptr;
}

int PctToValue(int pct, DWORD min, DWORD max) {
    pct = std::clamp(pct, 0, 100);
    return (int)std::lround(min + (max - min) * pct / 100.0);
}

}  // namespace

// ── Ddc ───────────────────────────────────────────────────────

std::vector<Monitor> Ddc::Enumerate() {
    std::vector<Monitor> out;
    std::vector<CachedMonitor> all = EnumerateAll();
    for (auto& cm : all) {
        Monitor m;
        m.deviceName   = cm.deviceName;
        m.description  = cm.description;
        m.ddcSupported = true;
        m.features     = cm.features;
        m.hasBrightness = cm.hasBrightness;
        m.brightnessMin = cm.bMin; m.brightnessCur = cm.bCur; m.brightnessMax = cm.bMax;
        m.hasContrast = cm.hasContrast;
        m.contrastMin = cm.cMin; m.contrastCur = cm.cCur; m.contrastMax = cm.cMax;
        m.hasVolume = HasCode(cm, kVcpAudioVolume);
        m.hasPower  = HasCode(cm, kVcpPower);
        m.hasInputs = HasCode(cm, kVcpInputSource);
        m.inputs    = DefinedValues(cm, kVcpInputSource);

        // 音量/输入源当前值（尽力读取）
        if (m.hasVolume) {
            DWORD cur = 0, mx = 0, err = ERROR_SUCCESS;
            if (TryDdc([&] { return GetVCPFeatureAndVCPFeatureReply(cm.hPhysical, kVcpAudioVolume, nullptr, &cur, &mx); }, err)) {
                m.volumeCur = cur;
                m.volumeMax = mx > 0 ? mx : 100;
            }
        }
        if (m.hasInputs) {
            DWORD cur = 0, mx = 0, err = ERROR_SUCCESS;
            if (TryDdc([&] { return GetVCPFeatureAndVCPFeatureReply(cm.hPhysical, kVcpInputSource, nullptr, &cur, &mx); }, err))
                m.inputCur = cur;
        }
        out.push_back(std::move(m));
    }
    DestroyAll(all);
    return out;
}

bool Ddc::GetVCP(const std::wstring& deviceName, BYTE code, DWORD& cur, DWORD& max) {
    if (!EnsureCache())
        return false;
    CachedMonitor* m = Find(deviceName);
    if (!m)
        return false;
    DWORD err = ERROR_SUCCESS;
    return TryDdc([&] {
        return GetVCPFeatureAndVCPFeatureReply(m->hPhysical, code, nullptr, &cur, &max);
    }, err);
}

bool Ddc::SetVCP(const std::wstring& deviceName, BYTE code, DWORD value) {
    if (!EnsureCache())
        return false;
    CachedMonitor* m = Find(deviceName);
    if (!m)
        return false;
    DWORD err = ERROR_SUCCESS;
    bool ok = TryDdc([&] { return SetVCPFeature(m->hPhysical, code, value); }, err);
    if (!ok)
        InvalidateCache();
    return ok;
}

bool Ddc::SetBrightnessPercent(const std::wstring& deviceName, int percent) {
    if (!EnsureCache())
        return false;
    CachedMonitor* m = Find(deviceName);
    if (!m || !m->hasBrightness)
        return false;
    return SetVCP(deviceName, kVcpLuminance, (DWORD)PctToValue(percent, m->bMin, m->bMax));
}

bool Ddc::SetContrastPercent(const std::wstring& deviceName, int percent) {
    if (!EnsureCache())
        return false;
    CachedMonitor* m = Find(deviceName);
    if (!m || !m->hasContrast)
        return false;
    return SetVCP(deviceName, kVcpContrast, (DWORD)PctToValue(percent, m->cMin, m->cMax));
}

int Ddc::AdjustAll(int target, int stepPercent) {
    if (!EnsureCache())
        return 0;
    int okCount = 0;
    for (auto& m : g_cache) {
        if (target == 2 && !HasCode(m, kVcpAudioVolume))
            continue;
        DWORD minv = 0, curv = 0, maxv = 0, err = ERROR_SUCCESS;

        if (target == 0) {
            if (!m.hasBrightness) continue;
            minv = m.bMin; maxv = m.bMax;
            if (!TryDdc([&] { return GetMonitorBrightness(m.hPhysical, &minv, &curv, &maxv); }, err)) continue;
        } else if (target == 1) {
            if (!m.hasContrast) continue;
            minv = m.cMin; maxv = m.cMax;
            if (!TryDdc([&] { return GetMonitorContrast(m.hPhysical, &minv, &curv, &maxv); }, err)) continue;
        } else {   // 音量 0x62，0..max
            minv = 0; maxv = 100;
            if (!TryDdc([&] { return GetVCPFeatureAndVCPFeatureReply(m.hPhysical, kVcpAudioVolume, nullptr, &curv, &maxv); }, err)) continue;
            if (maxv == 0) maxv = 100;
        }

        int curPct = (int)std::lround((curv - minv) * 100.0 / (maxv - minv));
        int targetPct = std::clamp(curPct + stepPercent, 0, 100);
        DWORD newVal = (DWORD)PctToValue(targetPct, minv, maxv);
        BYTE code = target == 0 ? kVcpLuminance : target == 1 ? kVcpContrast : kVcpAudioVolume;
        DWORD err2 = ERROR_SUCCESS;
        if (TryDdc([&] { return SetVCPFeature(m.hPhysical, code, newVal); }, err2))
            okCount++;
    }
    if (okCount < (int)g_cache.size())
        InvalidateCache();
    return okCount;
}

void Ddc::InvalidateCache() {
    DestroyAll(g_cache);
}

}  // namespace ddc
