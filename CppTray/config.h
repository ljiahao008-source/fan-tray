#pragma once
// 应用设置：exe 旁 config.json（极简 JSON，字段与旧版兼容）

struct AppConfig {
    int RefreshIntervalMs = 1000;
    bool AutoStart = false;
};

// 读取 config.json，失败/损坏回退默认值；间隔非法（<250 或 >60000）回退 1000。
AppConfig LoadConfig();

// 写入 config.json，返回是否成功。
bool SaveConfig(const AppConfig& cfg);
