/* 离线自检用的最小 esp_log.h 替身：把 ESP_LOGx 打到 stdout/stderr。
 * 用 gcc 的 ##__VA_ARGS__ 扩展（MinGW gcc 支持）。 */
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...)  fprintf(stderr, "E %s: " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)  fprintf(stdout, "W %s: " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)  fprintf(stdout, "I %s: " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  do { } while (0)
#define ESP_LOGV(tag, fmt, ...)  do { } while (0)
