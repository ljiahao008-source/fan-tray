// 锁屏设置业务层（一）：子进程执行 / 电源方案 / 屏幕保护
// 移植自 C# 版 src/gui/{Run,Power,Saver}.cs —— 行为逐项对齐

#include "lockscreen.h"

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <algorithm>

namespace lockscreen {

namespace {

// ---------- 编码：UTF-8 优先，非法序列回落 GBK(936)（powercfg/reg 输出为 GBK）----------

std::wstring DecodeBytes(const std::vector<char>& data) {
    if (data.empty())
        return L"";

    const char* p = data.data();
    int len = (int)data.size();

    // UTF-8 BOM
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        len -= 3;
    }

    // 严格 UTF-8 尝试（MB_ERR_INVALID_CHARS：遇到非法序列即失败）
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (wlen <= 0) {
        cp = 936;   // GBK
        flags = 0;
        wlen = MultiByteToWideChar(936, 0, p, len, nullptr, 0);
    }
    if (wlen <= 0)
        return L"";

    std::wstring out((size_t)wlen, L'\0');
    MultiByteToWideChar(cp, flags, p, len, &out[0], wlen);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
        b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        e--;
    return s.substr(b, e - b);
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

// 从行尾提取数字（0x 十六进制或十进制）；0xfffffffe 及以上视为"永不"= 0
std::optional<int> ParseTrailingNumber(const std::wstring& line) {
    std::wstring t = Trim(line);
    if (t.empty())
        return std::nullopt;

    size_t end = t.size();
    size_t start = end;
    while (start > 0 && (iswxdigit(t[start - 1]) || t[start - 1] == L'x' || t[start - 1] == L'X'))
        start--;
    if (start == end)
        return std::nullopt;

    std::wstring num = t.substr(start, end - start);
    long long v = 0;
    if (num.size() > 2 && (num[0] == L'0') && (num[1] == L'x' || num[1] == L'X')) {
        v = wcstoll(num.c_str() + 2, nullptr, 16);
    } else {
        v = wcstoll(num.c_str(), nullptr, 10);
    }
    if (v < 0)
        return std::nullopt;
    if (v >= 0xfffffffeLL)   // 0xffffffff = 永不
        return 0;
    return (int)v;
}

// ---------- 注册表小工具 ----------

bool RegReadString(HKEY root, const wchar_t* path, const wchar_t* name, std::wstring& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;
    out = buf;
    return true;
}

bool RegReadDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    DWORD v = 0, size = sizeof(v), type = 0;
    LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&v, &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || type != REG_DWORD)
        return false;
    out = v;
    return true;
}

bool RegWriteDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS st = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

bool RegWriteString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS st = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                                (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

// 把子进程输出压成单行错误文本（超 200 字符截断），便于显示
std::wstring ErrText(const wchar_t* what, const std::wstring& output) {
    std::wstring t = Trim(output);
    if (t.empty()) {
        std::wstring w = what;
        return w + L" 调用失败";
    }
    std::wstring one;
    for (wchar_t c : t) {
        if (c == L'\r' || c == L'\n') {
            if (!one.empty() && one.back() != L' ')
                one.push_back(L' ');
        } else {
            one.push_back(c);
        }
    }
    one = Trim(one);
    if (one.size() > 200)
        one = one.substr(0, 200) + L"…";
    return one;
}

constexpr wchar_t kPowerSchemeKey[] = L"SCHEME_CURRENT";

}  // namespace

// ==================== 子进程执行 ====================

ExecResult ExecCapture(const std::wstring& exe, const std::vector<std::wstring>& args) {
    ExecResult res;
    res.code = INT_MIN;

    std::wstring cmd = L"\"" + exe + L"\"";
    for (const auto& a : args) {
        cmd += L" \"";
        // 参数中可能含引号：简单转义为 \"（本模块参数均为路径/数值/关键字，风险极低）
        for (wchar_t c : a) {
            cmd.push_back(c);
            if (c == L'"')
                cmd.push_back(L'\\');
        }
        cmd += L"\"";
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return res;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;      // stdout/stderr 合并到同一管道，简化读取
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);        // 父进程必须关闭写端，否则读不会收到 EOF
    if (!ok) {
        CloseHandle(rd);
        return res;
    }

    std::vector<char> buf;
    char chunk[4096];
    DWORD read = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &read, nullptr) && read > 0)
        buf.insert(buf.end(), chunk, chunk + read);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, 20000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    res.code = (int)exitCode;
    res.text = Trim(DecodeBytes(buf));
    return res;
}

// ==================== 时长格式化 / 解析 ====================

std::wstring FormatSeconds(int sec) {
    if (sec <= 0)
        return L"永不";
    wchar_t buf[64] = {};
    if (sec < 60) {
        swprintf_s(buf, L"%d 秒", sec);
        return buf;
    }
    if (sec < 3600) {
        double m = sec / 60.0;
        if (m == (double)(int)m)
            swprintf_s(buf, L"%d 分钟", (int)m);
        else
            swprintf_s(buf, L"%.1f 分钟", m);
        return buf;
    }
    double h = sec / 3600.0;
    if (h == (double)(int)h)
        swprintf_s(buf, L"%d 小时", (int)h);
    else
        swprintf_s(buf, L"%.1f 小时", h);
    return buf;
}

std::optional<int> ParseValue(const std::wstring& raw) {
    std::wstring s = Trim(raw);
    if (s.empty())
        return std::nullopt;

    std::wstring lower;
    for (wchar_t c : s)
        lower.push_back((wchar_t)towlower(c));

    if (lower == L"never" || lower == L"off" || lower == L"no" || lower == L"false" ||
        lower == L"none" || lower == L"0" || lower == L"永不" || lower == L"从不" ||
        lower == L"关闭" || lower == L"禁用")
        return 0;

    // 提取前导数字
    size_t i = 0;
    while (i < lower.size() && (iswdigit(lower[i]) || lower[i] == L'.'))
        i++;
    if (i == 0) {
        // 纯数字路径之外的未知形式
        return std::nullopt;
    }
    double num = wcstod(lower.substr(0, i).c_str(), nullptr);
    std::wstring unit = Trim(lower.substr(i));
    unit.erase(std::remove(unit.begin(), unit.end(), L' '), unit.end());

    if (unit.empty() || unit == L"秒" || unit == L"s" || unit == L"sec" || unit == L"secs" ||
        unit == L"second" || unit == L"seconds")
        return (int)(num + 0.5);
    if (unit == L"分钟" || unit == L"分" || unit == L"m" || unit == L"min" || unit == L"mins" ||
        unit == L"minute" || unit == L"minutes")
        return (int)(num * 60 + 0.5);
    if (unit == L"小时" || unit == L"时" || unit == L"h" || unit == L"hr" || unit == L"hrs" ||
        unit == L"hour" || unit == L"hours")
        return (int)(num * 3600 + 0.5);
    if (unit.empty())
        return (int)(num + 0.5);
    return std::nullopt;
}

// ==================== 电源设置 ====================

namespace {

const PowerItemInfo kPowerItems[] = {
    { PowerKey::Lock, L"锁屏后黑屏", L"SUB_VIDEO", L"VIDEOCONLOCK",
      L"锁屏壁纸出现后多久自动关闭显示器（黑屏）。设为「永不」= 锁屏后屏幕一直亮。" },
    { PowerKey::Display, L"关闭显示器", L"SUB_VIDEO", L"VIDEOIDLE",
      L"无任何操作闲置多久后关闭屏幕。" },
    { PowerKey::Sleep, L"睡眠", L"SUB_SLEEP", L"STANDBYIDLE",
      L"闲置多久后进入睡眠。睡眠唤醒后仍需登录，也会看到锁屏界面。" },
    { PowerKey::Hibernate, L"休眠", L"SUB_SLEEP", L"HIBERNATEIDLE",
      L"闲置多久后进入休眠。需系统已开启休眠功能，否则此项不会生效。" },
};

bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

}  // namespace

const PowerItemInfo& PowerItem(PowerKey k) {
    return kPowerItems[(int)k];
}

const wchar_t* PowerKeyName(PowerKey k) {
    return kPowerItems[(int)k].name;
}

bool PowerManager::QueryAll(std::vector<std::pair<PowerKey, PowerState>>& out, std::wstring& err) {
    out.clear();
    ExecResult r = ExecCapture(L"powercfg.exe", { L"/q", kPowerSchemeKey });
    if (r.code == INT_MIN) {
        err = L"无法启动 powercfg（系统组件缺失或被安全软件拦截）";
        return false;
    }

    // 逐行解析：GUID 别名 行切换当前 setting；交流/直流索引行填值
    std::vector<std::wstring> lines;
    {
        std::wstring cur;
        for (wchar_t c : r.text) {
            if (c == L'\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
    }

    std::wstring curSetting;
    for (const auto& raw : lines) {
        std::wstring line = Trim(raw);

        size_t aliasPos = line.find(L"GUID ");
        if (aliasPos != std::wstring::npos &&
            (Contains(line, L"别名") || Contains(line, L"Alias"))) {
            size_t colon = line.find(L':');
            if (colon != std::wstring::npos) {
                curSetting = Trim(line.substr(colon + 1));
                // 去掉可能的前后引号/尖括号
                if (!curSetting.empty() && (curSetting.front() == L'"' || curSetting.front() == L'<'))
                    curSetting.erase(curSetting.begin());
                if (!curSetting.empty() && (curSetting.back() == L'"' || curSetting.back() == L'>'))
                    curSetting.pop_back();
            }
            continue;
        }

        bool isAc = Contains(line, L"当前交流电源设置索引") || Contains(line, L"Current AC Power Setting Index");
        bool isDc = Contains(line, L"当前直流电源设置索引") || Contains(line, L"Current DC Power Setting Index");
        if (!isAc && !isDc)
            continue;
        if (curSetting.empty())
            continue;

        auto num = ParseTrailingNumber(line);
        if (!num)
            continue;

        for (const auto& it : kPowerItems) {
            if (_wcsicmp(curSetting.c_str(), it.setting) != 0)
                continue;
            // 合并到 out（保留已有的一侧）
            PowerState* slot = nullptr;
            for (auto& p : out) {
                if (p.first == it.key) {
                    slot = &p.second;
                    break;
                }
            }
            if (!slot) {
                out.emplace_back(it.key, PowerState{});
                slot = &out.back().second;
            }
            if (isAc)
                slot->ac = *num;
            else
                slot->dc = *num;
        }
    }

    // 补齐未出现的项（例如休眠未开启时 HIBERNATEIDLE 不在输出中）
    for (const auto& it : kPowerItems) {
        bool found = false;
        for (const auto& p : out)
            if (p.first == it.key)
                found = true;
        if (!found)
            out.emplace_back(it.key, PowerState{});
    }
    return true;
}

PowerState PowerManager::QueryItem(PowerKey k, std::wstring& err) {
    std::vector<std::pair<PowerKey, PowerState>> all;
    if (!QueryAll(all, err))
        return PowerState{};
    for (const auto& p : all)
        if (p.first == k)
            return p.second;
    return PowerState{};
}

bool PowerManager::SetItem(PowerKey k, int seconds, const char* mode, bool activate,
                           std::vector<std::wstring>& errors) {
    const PowerItemInfo& info = PowerItem(k);
    std::string m = mode ? mode : "both";
    for (auto& c : m)
        c = (char)tolower((unsigned char)c);

    if (m != "dc") {
        ExecResult r = ExecCapture(L"powercfg.exe",
                                   { L"/setacvalueindex", kPowerSchemeKey, info.sub, info.setting,
                                     std::to_wstring(seconds) });
        if (r.code != 0)
            errors.push_back(std::wstring(L"接通电源: ") + ErrText(L"powercfg", r.text));
    }
    if (m != "ac") {
        ExecResult r = ExecCapture(L"powercfg.exe",
                                   { L"/setdcvalueindex", kPowerSchemeKey, info.sub, info.setting,
                                     std::to_wstring(seconds) });
        if (r.code != 0)
            errors.push_back(std::wstring(L"使用电池: ") + ErrText(L"powercfg", r.text));
    }
    if (activate && errors.empty()) {
        std::wstring err;
        if (!Activate(err))
            errors.push_back(std::wstring(L"使设置生效: ") + err);
    }
    return errors.empty();
}

bool PowerManager::Activate(std::wstring& err) {
    std::wstring name, guid;
    ActiveScheme(name, guid);
    ExecResult r = ExecCapture(L"powercfg.exe", { L"/setactive", guid });
    if (r.code == 0)
        return true;
    err = ErrText(L"powercfg /setactive", r.text);
    return false;
}

bool PowerManager::RestoreDefaults(std::wstring& err) {
    ExecResult r = ExecCapture(L"powercfg.exe", { L"-restoredefaultschemes" });
    if (r.code == 0)
        return true;
    err = ErrText(L"powercfg -restoredefaultschemes", r.text);
    return false;
}

bool PowerManager::ActiveScheme(std::wstring& name, std::wstring& guid) {
    guid = L"SCHEME_CURRENT";
    name = L"未知";
    ExecResult r = ExecCapture(L"powercfg.exe", { L"/getactivescheme" });
    if (r.code == INT_MIN || r.text.empty())
        return false;

    // GUID：8-4-4-4-12 十六进制
    const std::wstring& t = r.text;
    for (size_t i = 0; i + 36 <= t.size(); i++) {
        bool ok = true;
        for (int j = 0; j < 36; j++) {
            wchar_t c = t[i + j];
            if (j == 8 || j == 13 || j == 18 || j == 23) {
                if (c != L'-') { ok = false; break; }
            } else if (!iswxdigit(c)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            guid = t.substr(i, 36);
            // 括号内的方案名
            size_t open = t.find(L'(', i + 36);
            size_t close = (open == std::wstring::npos) ? std::wstring::npos : t.find(L')', open);
            if (open != std::wstring::npos && close != std::wstring::npos && close > open + 1) {
                std::wstring n = Trim(t.substr(open + 1, close - open - 1));
                if (!n.empty())
                    name = n;
            } else {
                // 无括号：取 GUID 之后整行剩余文本
                size_t lineEnd = t.find(L'\n', i + 36);
                std::wstring rest = Trim(t.substr(i + 36, lineEnd == std::wstring::npos
                                                               ? std::wstring::npos
                                                               : lineEnd - (i + 36)));
                if (!rest.empty())
                    name = rest;
            }
            return true;
        }
    }
    return false;
}

bool PowerManager::HasBattery() {
    SYSTEM_POWER_STATUS st{};
    if (!GetSystemPowerStatus(&st))
        return false;
    return (st.BatteryFlag & 0x80) == 0;   // 0x80 = 无系统电池
}

bool PowerManager::HibernateAvailable() {
    wchar_t drive[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"SystemDrive", drive, MAX_PATH);
    std::wstring path = (n > 0) ? std::wstring(drive) : L"C:";
    path += L"\\hiberfil.sys";
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

// ==================== 屏幕保护 ====================

namespace {
constexpr wchar_t kDesktopKey[] = L"Control Panel\\Desktop";
// 注意：SPI_SETSCREENSAVETIMEOUT / SPI_SETSCREENSAVEACTIVE 由 windows.h 提供，勿重复定义
constexpr UINT kSpiFlags = SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE;
}  // namespace

SaverState SaverManager::Get() {
    SaverState s;
    std::wstring active, timeout, secure;
    if (RegReadString(HKEY_CURRENT_USER, kDesktopKey, L"ScreenSaveActive", active))
        s.active = (active == L"1");
    if (RegReadString(HKEY_CURRENT_USER, kDesktopKey, L"ScreenSaveTimeOut", timeout))
        s.timeout = _wtoi(timeout.c_str());
    if (RegReadString(HKEY_CURRENT_USER, kDesktopKey, L"ScreenSaverIsSecure", secure))
        s.secure = (secure == L"1");
    if (s.timeout < 0)
        s.timeout = 0;
    return s;
}

bool SaverManager::Set(bool active, std::optional<int> timeout, std::optional<bool> secure,
                       std::vector<std::wstring>& errors) {
    if (!RegWriteString(HKEY_CURRENT_USER, kDesktopKey, L"ScreenSaveActive", active ? L"1" : L"0"))
        errors.push_back(L"写入注册表 ScreenSaveActive 失败（权限不足？）");
    if (timeout) {
        if (!RegWriteString(HKEY_CURRENT_USER, kDesktopKey, L"ScreenSaveTimeOut",
                            std::to_wstring(*timeout)))
            errors.push_back(L"写入注册表 ScreenSaveTimeOut 失败");
    }
    if (secure) {
        if (!RegWriteString(HKEY_CURRENT_USER, kDesktopKey, L"ScreenSaverIsSecure",
                            *secure ? L"1" : L"0"))
            errors.push_back(L"写入注册表 ScreenSaverIsSecure 失败");
    }

    // 通知系统即时生效；timeout 未提供时绝不能调 SPI_SETSCREENSAVETIMEOUT（传 0 会清掉用户设置）
    if (timeout)
        SystemParametersInfoW(SPI_SETSCREENSAVETIMEOUT, (UINT)*timeout, nullptr, kSpiFlags);
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, active ? 1 : 0, nullptr, kSpiFlags);
    return errors.empty();
}

}  // namespace lockscreen
