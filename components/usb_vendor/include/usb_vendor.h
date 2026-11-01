// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · WS-webusb 的 **两个 vendor 功能**（I1 注册入口）
//
// 分配（docs/contracts/frozen-allocation.md §1/§2）：
//   IF1 · Vendor 视频   : FF/00/00，**只有 1 个 bulk IN**（EP3，HS 512 / FS 64），**不发 WCID**
//   IF2 · Vendor 控制   : FF/00/00，bulk IN + bulk OUT（EP4，HS 512 / FS 64），**唯一发 WCID**
//
// ⚠️ 接口号 / 端点号 / 字符串索引**全部**来自 `usb_fn_alloc_t`，本组件**不硬编码**任何 IF/EP 号。
// ⚠️ 描述符片段只由本组件生成，**不得**碰配置描述符整体（I1 契约）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 诊断统计（LOG/TELEM 之外的自有计数） */
typedef struct {
    uint32_t video_frames_sent;    /**< 完整发出的编码帧数 */
    uint32_t video_frames_dropped; /**< 因端点忙/部分写入被丢的帧数 */
    uint32_t video_bytes_sent;     /**< 视频载荷字节数（不含帧头） */
    uint32_t video_stale_skips;    /**< 因 IN 端点还有残留字节而跳过的帧数（§4.1-3） */
    uint32_t ctrl_out_events;      /**< EP4 OUT 的 on_out 回调次数 */
    uint32_t ctrl_out_bytes;       /**< 从 EP4 OUT 读出的字节数 */
    bool     endpoints_bound;      /**< 两个功能的 init() 都已拿到分配结果 */
} usb_vendor_stats_t;

/**
 * @brief 注册两个 vendor 功能（**顺序固定：先视频 IF1、后控制 IF2**）。
 *
 * 必须在 `usb_composite_prepare()` / `usb_composite_start()` **之前**调用，
 * 且在 MTP（IF0）之后、NCM（IF3/IF4）之前 —— 接口号 = 注册顺序。
 *
 * @return ESP_OK / 复合层返回的错误
 */
esp_err_t usb_vendor_register(void);

/** 两个功能是否都已注册 */
bool usb_vendor_registered(void);

/**
 * @brief 启动本工作流的数据面：`webusb_proto_start()` + 视频发送任务。
 *
 * 幂等。可在 `usb_composite_start()` 之前或之后调用（任务自己等 mount）。
 */
esp_err_t usb_vendor_start(void);

void usb_vendor_get_stats(usb_vendor_stats_t *out);

#ifdef __cplusplus
}
#endif
