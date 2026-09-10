#pragma once
// 开机自启：计划任务（schtasks /RL HIGHEST，登录即提权不弹 UAC）
// 对照 AppSettings.cs（旧版 HKCU Run 项一并清理）

#include <string>

// 自启计划任务是否已存在
bool IsAutoStartTaskInstalled();

// enable=true 创建计划任务；false 删除（任务不存在视为已移除）。
// 返回 false 表示操作失败（需要管理员权限）。
bool ApplyAutoStart(bool enable);
