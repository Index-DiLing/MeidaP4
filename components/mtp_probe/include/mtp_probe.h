// SPDX-License-Identifier: Apache-2.0
//
// 探测阶段 · MTP 后端（片内 FATFS 卷，非 SD 卡）
//
// 生产者：mtp-core。要求见 docs/probe-contract.md §3.2。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂载 storage 分区（FATFS）并把它注册为 MTP 存储。
 *
 * 必须在 usb_probe_start() 之前调用：MTP 类驱动需要先 install，TinyUSB 再安装。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED 当 CONFIG_TINYUSB_MTP_ENABLED 关闭时。
 */
esp_err_t mtp_probe_start(void);

/** @brief 卷是否已挂载。 */
bool mtp_probe_is_mounted(void);

/** @brief 返回挂载点字符串（编译期常量，不需要释放）。 */
const char *mtp_probe_mount_point(void);

#ifdef __cplusplus
}
#endif
