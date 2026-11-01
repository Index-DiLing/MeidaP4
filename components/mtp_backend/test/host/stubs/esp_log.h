// 主机侧离线测试专用 stub：ESP-IDF 的 esp_log.h（把日志打到 stdout）
// ⚠️ 只在 components/mtp_backend/test/host/ 里使用，绝不进固件构建。
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) do { printf("[E] %-12s " fmt "\n", (tag), ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { printf("[W] %-12s " fmt "\n", (tag), ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { printf("[I] %-12s " fmt "\n", (tag), ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
