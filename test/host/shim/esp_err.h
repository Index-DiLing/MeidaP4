// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 原生 gcc 离线测试用的最小 esp_err.h 垫片（verifier，test/host 专用）
//
// 存在的唯一理由：`components/usb_composite/include/usb_composite.h` 冻结契约里
// `#include "esp_err.h"`，而离线测试用原生 gcc（无 ESP-IDF）。本垫片只提供
// **类型 + 本测试用到的错误码**，数值与 ESP-IDF v6.1
// `components/esp_common/include/esp_err.h` **逐条一致**（见每行注释）。
//
// ⚠️ 刻意**不**定义 IDF 里不存在的宏：如果产品代码用了 IDF 没有的 esp_err 宏，
//    离线编译必须和 IDF 编译一样报错（本垫片的价值之一就是把这种错误提前暴露）。

#pragma once

#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;

/* ---- 与 ESP-IDF v6.1 components/esp_common/include/esp_err.h 一致的取值 ---- */
#define ESP_OK                      0       /* IDF: 0 */
#define ESP_FAIL                    (-1)    /* IDF: -1 */

#define ESP_ERR_NO_MEM              0x101   /* IDF: 0x101 */
#define ESP_ERR_INVALID_ARG         0x102   /* IDF: 0x102 */
#define ESP_ERR_INVALID_STATE       0x103   /* IDF: 0x103 */
#define ESP_ERR_INVALID_SIZE        0x104   /* IDF: 0x104 */
#define ESP_ERR_NOT_FOUND           0x105   /* IDF: 0x105 */
#define ESP_ERR_NOT_SUPPORTED       0x106   /* IDF: 0x106 */
#define ESP_ERR_TIMEOUT             0x107   /* IDF: 0x107 */
#define ESP_ERR_INVALID_RESPONSE    0x108   /* IDF: 0x108 */
#define ESP_ERR_INVALID_CRC         0x109   /* IDF: 0x109 */
#define ESP_ERR_INVALID_VERSION     0x10A   /* IDF: 0x10A */
#define ESP_ERR_INVALID_MAC         0x10B   /* IDF: 0x10B */
#define ESP_ERR_NOT_FINISHED        0x10C   /* IDF: 0x10C */
#define ESP_ERR_NOT_ALLOWED         0x10D   /* IDF: 0x10D */

/* 离线测试下把 ESP_ERROR_CHECK 变成"打印 + abort"，便于定位 */
static inline void esp_err_shim_check(esp_err_t err, const char *expr, const char *file, int line)
{
    if (err != ESP_OK) {
        fprintf(stderr, "[esp_err shim] ESP_ERROR_CHECK(%s) -> 0x%X at %s:%d\n", expr, err, file, line);
        abort();
    }
}
#define ESP_ERROR_CHECK(x) esp_err_shim_check((x), #x, __FILE__, __LINE__)

/* esp_err_to_name 的离线替身（只覆盖本测试会遇到的码） */
static inline const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default: return "ESP_ERR_?";
    }
}
