// 主机侧离线测试专用 stub：ESP-IDF 的 esp_err.h
// ⚠️ 只在 components/mtp_backend/test/host/ 里使用（原生 gcc 编译），绝不进固件构建。
#pragma once

#include <stdint.h>

typedef int esp_err_t;

/* 与 IDF 的真实取值一致（esp_err.h 的 ESP_ERR_* 基表） */
#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_NOT_IMPLEMENTED 0x107

#ifdef __cplusplus
extern "C" {
#endif

const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif
