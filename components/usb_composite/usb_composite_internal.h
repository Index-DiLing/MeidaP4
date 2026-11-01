// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I1 复合层**内部**接口（非冻结，Lead 独占）
//
// ⚠️ 本头文件服务的 `usb_composite_desc.c` 是**纯计算**：它不碰 TinyUSB、不碰任何 ESP-IDF 运行时，
//    只用 `<string.h>` / `<stdio.h>` + `esp_err_t`（宿主侧由 test/host/shim/esp_err.h 提供）。
//    这样 test/host/test_composite_desc.c 能用**原生 gcc** 把它编出来做离线校验。
//    **不要**往 `usb_composite_desc.c` 里加 IDF 调用（日志、NVS、FreeRTOS、tinyusb 全部禁止）；
//    需要日志就通过 `uc_last_error()` 把原因交回调用方去打。
#pragma once

#include "usb_composite.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 注册 / 组装 ---------------- */

/** @see usb_composite_register。已 prepare 后返回 ESP_ERR_INVALID_STATE */
esp_err_t uc_register(const usb_function_t *fn);

/** @see usb_composite_set_identity。已 prepare 后返回 ESP_ERR_INVALID_STATE */
esp_err_t uc_set_identity(const usb_composite_identity_t *id);

/**
 * @brief 组装全部描述符 + 分配接口/端点/字符串 + 复算 FIFO 预算。幂等。
 *
 * 不做任何 I/O，不分配堆内存（全部 static）。
 */
esp_err_t uc_prepare(void);

bool uc_is_prepared(void);

/** 最近一次失败原因（静态缓冲，永不为 NULL；没人设过时返回空串） */
const char *uc_last_error(void);

/* ---------------- 自检 ---------------- */

/**
 * @brief 逐字节解析自己组装出来的描述符，与冻结分配表比对。
 *
 * @param[out] checks_run     执行的检查条数（可为 NULL）
 * @param[out] checks_failed  失败条数（可为 NULL）
 * @return ESP_OK（全部通过）/ ESP_ERR_INVALID_STATE（有失败，原因见 uc_last_error()）
 */
esp_err_t uc_selfcheck(uint32_t *checks_run, uint32_t *checks_failed);

/* ---------------- 描述符访问（供 tinyusb_desc_config_t 与离线 dump） ---------------- */

const uint8_t *uc_dev_desc(uint32_t *len);
const uint8_t *uc_qual_desc(uint32_t *len);
const uint8_t *uc_cfg_desc(bool high_speed, uint32_t *len);
const uint8_t *uc_bos_desc(uint32_t *len);
const uint8_t *uc_msos_desc(uint32_t *len);
const char *const *uc_str_desc(int *count);
const usb_composite_budget_t *uc_budget(void);

/* ---------------- 注册表查询 ---------------- */

int uc_fn_count(void);
const usb_function_t *uc_fn(int idx);
const usb_fn_alloc_t *uc_fn_alloc(int idx);

/** vendor 驱动序号 -> 功能下标；找不到返回 -1 */
int uc_fn_by_vendor_idx(uint8_t vendor_idx);

#ifdef __cplusplus
}
#endif
