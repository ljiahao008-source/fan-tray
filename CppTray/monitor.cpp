// GetIfTable2 / MIB_IF_ROW2 需要 Vista+ 目标版本声明（必须在任何 windows 头之前）
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "monitor.h"

#include <cwctype>
#include <cstdlib>
#include <vector>
#include <iphlpapi.h>
#include <cstdint>

namespace {

constexpr uint32_t kMsrIntelPowerUnit = 0x606;
constexpr uint32_t kMsrIntelPkgEnergy = 0x611;   // PKG_ENERGY_STATUS
constexpr uint32_t kMsrAmdPowerUnit = 0xC0010299;
constexpr uint32_t kMsrAmdPkgEnergy = 0xC001029B;

constexpr uint32_t kSmnAmdThmTconCurTmp = 0x00059800;   // AMD Zen 温度寄存器（对照 LHM Amd17Cpu）
constexpr uint32_t kTempRangeSelMask = 0x80000;
constexpr uint32_t kTempTjSelMask = 0x30000;

constexpr int kFanDebounceTicks = 3;             // 风扇连续失败 3 拍才算真无数据
constexpr ULONGLONG kPowerDeadReviveMs = 60000;  // 功耗持续 0/空 60s → 自愈

constexpr wchar_t kCpuNameValue[] = L"ProcessorNameString";

// 注册表读取 CPU 型号名称（HKLM\HARDWARE\DESCRIPTION 在 32 位视图差异，走 64 位视图）
std::wstring ReadCpuNameImpl() {
    HKEY key = nullptr;
    std::wstring result;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return result;
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(key, kCpuNameValue, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
        result = buf;
    RegCloseKey(key);
    return result;
}

bool EndsWithSuffix(const std::wstring& name, size_t digitsEnd, const wchar_t* suffix) {
    // 数字段结束后紧跟的字母段匹配 suffix（如 1360P → P；7945HX → HX）
    size_t i = digitsEnd;
    while (i < name.size() && iswalpha(name[i]))
        i++;
    size_t len = i - digitsEnd;
    if (len == 0)
        return false;
    size_t slen = wcslen(suffix);
    if (len != slen)
        return false;
    return _wcsnicmp(name.c_str() + digitsEnd, suffix, slen) == 0;
}

}  // namespace

MonitorCore::MonitorCore() {
    InitializeCriticalSection(&_lock);
    QueryPerformanceFrequency(&_freq);
}

MonitorCore::~MonitorCore() {
    _pawn.Close();
    _fanDev.Close();
    DeleteCriticalSection(&_lock);
}

bool MonitorCore::Init() {
    bool ok = SetupPawnPower();
    _pawnOk = ok;
    _fanDev.Init();   // 失败则风扇 "--"，不阻塞
    return _pawnOk;
}

bool MonitorCore::SetupPawnPower() {
    // 按 CPU vendor 选择固件模块与 RAPL MSR：
    //   Intel：IntelMSR.bin + 0x606/0x611
    //   AMD（Zen，Family17h 体系）：AMDFamily17.bin + 0xC0010299/0xC001029B
    bool isAmd = ReadCpuNameImpl().find(L"AMD") != std::wstring::npos ||
                 ReadCpuNameImpl().find(L"Ryzen") != std::wstring::npos;
    _isAmd = isAmd;

    WORD resId = isAmd ? 103 : 101;               // IDR_AMDFAMILY17 / IDR_INTELMSR
    uint32_t unitMsr = isAmd ? kMsrAmdPowerUnit : kMsrIntelPowerUnit;
    _pkgMsr = isAmd ? kMsrAmdPkgEnergy : kMsrIntelPkgEnergy;

    if (!_pawn.LoadModuleFromResource(nullptr, resId))
        return false;

    uint32_t eax = 0, edx = 0;
    if (!_pawn.ReadMsr(unitMsr, eax, edx))
        return false;

    // ESU [12:8]：每计数 1/2^ESU 焦耳（默认 ESU=6 ≈ 15.3µJ；AMD 常用 ESU=16）
    int esu = (int)((eax >> 8) & 0x1F);
    if (esu > 30)
        esu = 6;
    _mult = 1.0 / (double)(1LL << esu);
    _hasLastEnergy = false;
    return true;
}

bool MonitorCore::ReadPackagePower(float& watts) {
    if (!_pawnOk)
        return false;
    uint32_t eax = 0, edx = 0;
    if (!_pawn.ReadMsr(_pkgMsr, eax, edx))
        return false;

    uint32_t energy = eax;   // 能量计数器（低 32 位）
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (_hasLastEnergy) {
        double dt = (double)(now.QuadPart - _lastTime.QuadPart) / (double)_freq.QuadPart;
        if (dt < 0.01)
            return false;
        uint32_t delta = (uint32_t)(energy - _lastEnergy);   // 无符号回绕
        watts = (float)(_mult * (double)delta / dt);
        _lastEnergy = energy;
        _lastTime = now;
        return true;
    }

    _lastEnergy = energy;
    _lastTime = now;
    _hasLastEnergy = true;
    return false;   // 第一拍只做基准，无功耗值
}

bool MonitorCore::ReadCpuUsage(float& pct) {
    FILETIME idleFt{}, kernelFt{}, userFt{};
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt))
        return false;

    ULONGLONG idle = ((ULONGLONG)idleFt.dwHighDateTime << 32) | idleFt.dwLowDateTime;
    ULONGLONG kernel = ((ULONGLONG)kernelFt.dwHighDateTime << 32) | kernelFt.dwLowDateTime;
    ULONGLONG user = ((ULONGLONG)userFt.dwHighDateTime << 32) | userFt.dwLowDateTime;

    if (!_hasLastSysTimes) {
        _lastIdle = idle;
        _lastKernel = kernel;
        _lastUser = user;
        _hasLastSysTimes = true;
        return false;   // 首拍只做基准
    }

    ULONGLONG dIdle = idle - _lastIdle;
    ULONGLONG dKernel = kernel - _lastKernel;   // kernel 时间已含 idle
    ULONGLONG dUser = user - _lastUser;
    _lastIdle = idle;
    _lastKernel = kernel;
    _lastUser = user;

    ULONGLONG total = dKernel + dUser;
    if (total == 0)
        return true;   // 同刻重复采样：占用视为 0，保持曲线连续（不算失败）
    double busy = (double)total - (double)dIdle;
    if (busy < 0)
        busy = 0;
    double v = busy * 100.0 / (double)total;
    if (v > 100.0)
        v = 100.0;
    pct = (float)v;
    return true;
}

bool MonitorCore::ReadCpuTemp(float& celsius) {
    // 仅 AMD（Zen）走 SMN；Intel 需 DTS（0x1A2）暂未实现 → 显示 "--"
    if (!_pawnOk || !_isAmd)
        return false;

    uint32_t raw = 0;
    if (!_pawn.ReadSmn(kSmnAmdThmTconCurTmp, raw))
        return false;

    // 对照 LHM Amd17Cpu：温度 = (raw >> 21) * 0.125 ℃；特定范围标记需减 49
    bool tempOffsetFlag = (raw & kTempRangeSelMask) != 0 || (raw & kTempTjSelMask) == kTempTjSelMask;
    float t = (float)((raw >> 21) * 125) * 0.001f;
    if (tempOffsetFlag)
        t -= 49.0f;

    if (t <= 0.f || t > 125.f)   // 越界视为无效（寄存器未就绪/机型不支持）
        return false;
    celsius = t;
    return true;
}

bool MonitorCore::ReadMemUsage(float& pct) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms))
        return false;
    pct = (float)ms.dwMemoryLoad;   // 物理内存占用百分比（与 C# 版同口径）
    return true;
}

bool MonitorCore::ReadNetSpeeds(float& downKBps, float& upKBps) {
    // 用经典 GetIfTable（MIB_IFROW，32 位计数）：无 SDK 版本门槛，1 秒采样下不会回绕
    DWORD size = 0;
    if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return false;

    std::vector<BYTE> buf(size, 0);
    MIB_IFTABLE* table = (MIB_IFTABLE*)buf.data();
    if (GetIfTable(table, &size, FALSE) != NO_ERROR)
        return false;

    ULONGLONG inOctets = 0, outOctets = 0;
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        const MIB_IFROW& r = table->table[i];
        if (r.dwType == IF_TYPE_SOFTWARE_LOOPBACK || r.dwType == IF_TYPE_TUNNEL)
            continue;                                     // 排除回环/隧道（VPN 虚拟口）
        if (r.dwOperStatus != IF_OPER_STATUS_OPERATIONAL)
            continue;                                     // 未连接/未启用
        inOctets += r.dwInOctets;
        outOctets += r.dwOutOctets;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!_hasLastNet) {
        _lastNetIn = inOctets;
        _lastNetOut = outOctets;
        _lastNetTime = now;
        _hasLastNet = true;
        return false;   // 首拍只做基准
    }

    double dt = (double)(now.QuadPart - _lastNetTime.QuadPart) / (double)_freq.QuadPart;
    uint32_t dIn = (uint32_t)(inOctets - _lastNetIn);      // 32 位计数器：无符号差值自然处理回绕
    uint32_t dOut = (uint32_t)(outOctets - _lastNetOut);
    _lastNetIn = inOctets;
    _lastNetOut = outOctets;
    _lastNetTime = now;

    if (dt < 0.05)
        return false;
    downKBps = (float)((double)dIn / dt / 1024.0);
    upKBps = (float)((double)dOut / dt / 1024.0);
    return true;
}

void MonitorCore::Sample(SampleSet& out) {
    // —— 功耗（读 MSR 放锁外）——
    float watts = 0.f;
    bool gotPower = ReadPackagePower(watts);
    if (gotPower && watts == 0.f)
        gotPower = false;   // 0 视为无效（自愈判定依据）

    if (gotPower) {
        _powerDeadSinceTick = 0;
    } else if (_powerDeadSinceTick == 0) {
        _powerDeadSinceTick = GetTickCount64();
    } else if (GetTickCount64() - _powerDeadSinceTick >= kPowerDeadReviveMs) {
        Revive();
        _powerDeadSinceTick = GetTickCount64();
    }

    // —— 风扇（WMI 最慢，放锁外；连续 3 拍失败才判定真无数据）——
    float rpm = _fanDev.ReadFanRpm();
    bool fanRepeated = false;
    if (rpm < 0.f) {
        if (++_fanFailStreak < kFanDebounceTicks && _hasLastFan) {
            rpm = _lastFan;
            fanRepeated = true;
        } else {
            _hasLastFan = false;
        }
    } else {
        _fanFailStreak = 0;
        _lastFan = rpm;
        _hasLastFan = true;
    }

    // —— CPU 占用 / 温度 / 内存 / 网速（均放锁外，单项失败不影响其他指标）——
    float usage = 0.f, temp = 0.f, memPct = 0.f, downKB = 0.f, upKB = 0.f;
    bool gotUsage = ReadCpuUsage(usage);
    bool gotTemp = ReadCpuTemp(temp);
    bool gotMem = ReadMemUsage(memPct);
    bool gotNet = ReadNetSpeeds(downKB, upKB);

    // —— 统计（与 C# Metric 同算法，锁内更新）——
    EnterCriticalSection(&_lock);
    auto Apply = [](Metric& m, float v, bool repeated) {
        if (!repeated) {
            if (!m.valid) { m.min = m.max = v; }
            else {
                if (v > m.max) m.max = v;
                if (v < m.min) m.min = v;
            }
            m.sum += v;
            m.count++;
            m.avg = (float)(m.sum / m.count);
        }
        m.current = v;
        m.valid = true;
    };
    auto Invalidate = [](Metric& m) { m.valid = false; };

    if (gotPower)
        Apply(_power, watts, /*repeated=*/false);
    else
        Invalidate(_power);

    if (rpm >= 0.f)
        Apply(_fan, rpm, fanRepeated);
    else
        Invalidate(_fan);

    if (gotUsage)
        Apply(_cpuUsage, usage, false);
    else
        Invalidate(_cpuUsage);

    if (gotTemp)
        Apply(_cpuTemp, temp, false);
    else
        Invalidate(_cpuTemp);

    if (gotMem)
        Apply(_mem, memPct, false);
    else
        Invalidate(_mem);

    if (gotNet) {
        Apply(_netDown, downKB, false);
        Apply(_netUp, upKB, false);
    } else {
        Invalidate(_netDown);
        Invalidate(_netUp);
    }

    out.power = _power;
    out.fan = _fan;
    out.cpuUsage = _cpuUsage;
    out.cpuTemp = _cpuTemp;
    out.mem = _mem;
    out.netDown = _netDown;
    out.netUp = _netUp;
    LeaveCriticalSection(&_lock);
}

void MonitorCore::ResetStats() {
    EnterCriticalSection(&_lock);
    _power.Reset();
    _fan.Reset();
    _cpuUsage.Reset();
    _cpuTemp.Reset();
    _mem.Reset();
    _netDown.Reset();
    _netUp.Reset();
    LeaveCriticalSection(&_lock);
}

Thresholds MonitorCore::GetThresholds() {
    std::wstring name = ReadCpuNameImpl();
    double tdp = EstimateTdp(name);
    Thresholds t;
    t.powerElevated = tdp;
    t.powerCritical = tdp * 1.6;
    t.fanElevated = 3200;
    t.fanCritical = 4800;
    // 占用/温度/内存为通用经验阈值（与 C# 版一致：绿 / 橙 / 红）
    t.usageElevated = 70;
    t.usageCritical = 90;
    t.tempElevated = 75;
    t.tempCritical = 90;
    t.memElevated = 70;
    t.memCritical = 90;
    return t;
}

double MonitorCore::EstimateTdp(const std::wstring& name) {
    if (name.empty())
        return 35.0;
    // 提取末尾数字段后的后缀（如 8745H / 7945HX / 1360P）
    for (size_t i = name.size(); i > 0; --i) {
        if (!iswdigit(name[i - 1]))
            continue;
        size_t end = i;   // 数字段后一位
        size_t start = i - 1;
        while (start > 0 && iswdigit(name[start - 1]))
            start--;
        size_t len = end - start;
        if (len >= 2 && len <= 5) {
            if (EndsWithSuffix(name, end, L"HX") || EndsWithSuffix(name, end, L"HK"))
                return 55.0;
            if (EndsWithSuffix(name, end, L"HS") || EndsWithSuffix(name, end, L"HQ") || EndsWithSuffix(name, end, L"H"))
                return 45.0;
            if (EndsWithSuffix(name, end, L"P"))
                return 28.0;
            if (EndsWithSuffix(name, end, L"U"))
                return 15.0;
        }
    }
    return 35.0;
}

void MonitorCore::Revive() {
    // 拉起 PawnIO 驱动服务（已提权进程执行 sc start 无副作用）
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"sc.exe start PawnIO";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    // 重建功耗引擎
    _pawn.Close();
    _pawnOk = SetupPawnPower();
}
