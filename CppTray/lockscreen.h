#pragma once
// 锁屏设置 —— 业务层（C++ 移植自 C# 版 src/gui/{Power,Saver,Dynlock,Config,Run}.cs）
// 职责：电源方案（powercfg）、屏幕保护（注册表+SPI）、动态锁（注册表）、配置导入导出、子进程执行
// 不含任何界面代码；所有失败以返回值/错误文本表达，不抛异常、不弹窗（由界面层决定如何提示）

#include <windows.h>
#include <string>
#include <vector>
#include <optional>

namespace lockscreen {

// ==================== 通用工具 ====================

// 执行子进程并返回 (退出码, 合并输出文本)。启动失败时 code = INT_MIN
struct ExecResult {
    int code = INT_MIN;
    std::wstring text;
};
ExecResult ExecCapture(const std::wstring& exe, const std::vector<std::wstring>& args);

// 秒 → 中文时长（永不 / N 秒 / N 分钟 / N 小时）
std::wstring FormatSeconds(int sec);
// 时间文本 → 秒（never/30s/10min/2h/600）；非法返回 nullopt
std::optional<int> ParseValue(const std::wstring& text);

// ==================== 电源设置 ====================

enum class PowerKey { Lock, Display, Sleep, Hibernate, Count };

struct PowerItemInfo {
    PowerKey key;
    const wchar_t* name;      // 锁屏后黑屏 / 关闭显示器 / 睡眠 / 休眠
    const wchar_t* sub;       // SUB_VIDEO / SUB_SLEEP
    const wchar_t* setting;   // VIDEOCONLOCK / VIDEOIDLE / STANDBYIDLE / HIBERNATEIDLE
    const wchar_t* desc;
};

const PowerItemInfo& PowerItem(PowerKey k);
const wchar_t* PowerKeyName(PowerKey k);

struct PowerState {
    std::optional<int> ac;    // 接通电源（秒）；nullopt = 读取不到（如休眠未开启）
    std::optional<int> dc;    // 使用电池（秒）
};

class PowerManager {
public:
    // 一次 powercfg /q SCHEME_CURRENT 解析全部设置
    static bool QueryAll(std::vector<std::pair<PowerKey, PowerState>>& out, std::wstring& err);
    static PowerState QueryItem(PowerKey k, std::wstring& err);

    // 设置单项；mode: "both" / "ac" / "dc"；activate=false 时批量设置后统一 Activate()
    static bool SetItem(PowerKey k, int seconds, const char* mode, bool activate,
                        std::vector<std::wstring>& errors);
    static bool Activate(std::wstring& err);
    static bool RestoreDefaults(std::wstring& err);
    static bool ActiveScheme(std::wstring& name, std::wstring& guid);

    // 环境检测
    static bool HasBattery();
    static bool HibernateAvailable();
};

// ==================== 屏幕保护 ====================

struct SaverState {
    bool active = false;
    int timeout = 900;
    bool secure = false;
};

class SaverManager {
public:
    static SaverState Get();
    // timeout/secure 为 nullopt 表示不改动该项（避免把用户原设置清成 0）
    static bool Set(bool active, std::optional<int> timeout, std::optional<bool> secure,
                    std::vector<std::wstring>& errors);
};

// ==================== 动态锁 ====================

struct DynlockState {
    bool enabled = false;              // EnableGoodbye
    bool deviceSelected = false;       // 信任设备是否有效
    int pairedCount = 0;               // 7 天内活动过的配对设备数
    int phoneCount = 0;                // 其中手机类设备数
    std::wstring selectedMac;          // DevicePairing
    std::wstring selectedName;         // 设备名（读不到时回退 MAC）
    std::wstring keysHealth;           // healthy / broken / unknown
    int keysAce = -1;                  // Keys 键 ACE 数
    bool lockdownOnLeave = false;      // Win11 25H2「离开时锁定」独立开关
};

class DynlockManager {
public:
    static DynlockState Get();
    // 开启时若无有效信任设备会自动补选（优先手机类）；warning 为"已生效但需知悉"，error 为彻底失败
    static bool Set(bool on, std::wstring& warning, std::wstring& error);
    static bool OpenSettings(const wchar_t* target);   // dynamiclock / bluetooth / signin
};

// ==================== 配置导入导出 ====================

class ConfigIO {
public:
    // 导出全部设置到 JSON 文件；返回 (ok, 路径或错误信息)
    static bool Export(const std::wstring& file, std::wstring& outPath, std::wstring& err);
    // 从 JSON 文件导入；results 为逐项结果，errors 为失败项
    static bool Import(const std::wstring& file, std::vector<std::wstring>& results,
                       std::vector<std::wstring>& errors);
    // 默认配置文件路径（exe 同目录 data\锁屏设置.json）
    static std::wstring DefaultConfigPath();
};

}  // namespace lockscreen
