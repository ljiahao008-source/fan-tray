#pragma once
// 应用设置：exe 旁 config.json（极简 JSON，字段与旧版兼容）
// 显示开关默认全开；旧配置文件缺字段时保持默认值（不回退为关闭）

struct AppConfig {
    int RefreshIntervalMs = 1000;
    bool AutoStart = false;

    // 任务栏显示项（单行排列，按顺序：功耗 风扇 占用 温度 内存 网速）
    bool ShowPower = true;
    bool ShowFan = true;
    bool ShowCpuUsage = true;
    bool ShowCpuTemp = true;
    bool ShowMem = true;
    bool ShowNet = true;
};

// 读取 config.json，失败/损坏回退默认值；间隔非法（<250 或 >60000）回退 1000。
AppConfig LoadConfig();

// 写入 config.json，返回是否成功。
bool SaveConfig(const AppConfig& cfg);
