// SPDX-License-Identifier: Apache-2.0
//
// 探测阶段 · USB 复合层（WebUSB + MS OS 2.0 / WCID + bulk 回环）
//
// 接口与端点分配见 docs/probe-contract.md §1/§2。**这些数字在探测期内不得单方面改动。**
//
// 生产者：usb-core
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 冻结的接口/端点分配（模式 A：单 Vendor；模式 B：MTP + Vendor） ---------- */
#define PROBE_ITF_MTP            0  /* 仅模式 B */
#define PROBE_ITF_VENDOR_A       0  /* 模式 A */
#define PROBE_ITF_VENDOR_B       1  /* 模式 B */

#define PROBE_EP_MTP_EVT_IN      1  /* interrupt IN,  64 B  */
#define PROBE_EP_MTP_DATA        2  /* bulk IN/OUT,  512 B  */
#define PROBE_EP_VENDOR_A        1  /* bulk IN/OUT,  512 B  */
#define PROBE_EP_VENDOR_B        3  /* bulk IN/OUT,  512 B  */

#define PROBE_VID                0x303A
#define PROBE_PID_MODE_A         0x4001
#define PROBE_PID_MODE_B         0x4002
#define PROBE_BCD_USB            0x0210  /* 必须 >= 0x0210，否则主机不取 BOS */

/* ---------- 统计 ---------- */
typedef struct {
    uint32_t out_bytes;     /* 主机 -> 设备，累计字节 */
    uint32_t in_bytes;      /* 设备 -> 主机，累计字节 */
    uint32_t out_packets;   /* 主机 -> 设备，packet 数 */
    uint32_t in_packets;    /* 设备 -> 主机，packet 数 */
    uint32_t echo_errors;   /* 回环不一致次数（设备侧无法自检，保留给主机上报） */
    uint32_t ctrl_requests; /* 收到的供应商控制请求数（WebUSB GET_URL / MS OS GET_MS_DESCRIPTOR） */
    uint32_t ctrl_stalls;   /* 未实现而 stall 的供应商请求数 */
    bool     mounted;       /* 主机是否已枚举完成 */
} usb_probe_stats_t;

/**
 * @brief 组装描述符、安装 TinyUSB（P4 HS 口）并启动回环任务。
 *
 * 多次调用返回 ESP_ERR_INVALID_STATE。调用前应先完成 mtp_probe_start()（模式 B）。
 */
esp_err_t usb_probe_start(void);

/**
 * @brief 取累计统计。任何线程可调用；out 为 NULL 时直接返回。
 */
void usb_probe_get_stats(usb_probe_stats_t *out);

/**
 * @brief 返回当前模式下配置描述符的总长度（字节），供离线校验与自检使用。
 *
 * @param high_speed true 取 HS 描述符，false 取 FS 描述符
 * @return 总长度；未实现时返回 0
 */
uint32_t usb_probe_config_desc_len(bool high_speed);

#ifdef __cplusplus
}
#endif
