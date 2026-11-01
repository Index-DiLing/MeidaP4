// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · FILE 通道设备侧服务（对外 API）
//
//   规约：docs/protocol.md §5.3（FILE 通道 8 字节子头 op/status/xid/offset）
//   语义权威：web/file.js（主机侧；设备侧必须逐 op 对齐）
//   载体：components/webusb_proto（可靠通道 W=8 / ACK / 重传）—— 本组件只注册 handler
//   写作用域：components/file_service/**（WS-F）
//
// 一句话：把 WebUSB FILE 通道映射到 TF 卡（FATFS，挂载点由 sd_storage 决定，当前 /sdcard）
// 上的真实文件操作，并跑一个"PC（MTP）↔ P4（FILE）文件级共享"探针。
//
// 生命周期：main/main_prod.cpp 在 MTP/SD 之后调用 file_service_start()，**必须早于
// `usb_vendor_start()`/`usb_composite_start()`** —— HELLO 的 caps 里 FILE 位由
// `webusb_proto_set_file_handler()` 是否注册决定，而 HELLO 在收到 HELLO_ACK 后就停发，
// 注册晚了主机可能缓存到一份"没有 FILE 位"的 caps。
//
// 失败策略：SD 未挂载时服务照常启动（FILE 请求统一回 WU_ST_EBUSY），绝不 abort / 绝不阻塞 USB。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 句柄表容量（协议/任务书要求 8）—— GET_STATS 的 handles_open 上限 */
#define FILE_SERVICE_MAX_HANDLES 8
/** LIST_RSP 单个目录项的名字域宽度（协议 §5.3：char name[256]） */
#define FILE_SERVICE_NAME_LEN 256
/** FILE op 总数（0..12，见 wu_codec.h 的 WU_FILE_*） */
#define FILE_SERVICE_OP_COUNT 13

/** FILE 服务统计（main_prod.cpp 的健康遥测 + 真机验收用） */
typedef struct {
    /* ---- main 已引用的字段（语义不得变） ---- */
    uint32_t requests;          /**< 收到并通过校验的 FILE 请求总数 */
    uint32_t responses;         /**< 成功排进 TX 队列的响应数 */
    uint32_t bytes_read;        /**< READ_RSP 实际送出的文件数据字节 */
    uint32_t bytes_written;     /**< WRITE_REQ 实际写入磁盘的字节 */
    uint32_t errors;            /**< status != OK 的响应数 */
    uint32_t handles_open;      /**< 当前打开的句柄数（0..8） */
    uint32_t list_pages;        /**< 成功发出的 LIST_RSP 页数 */
    uint32_t share_probe_hits;  /**< 探针检测到 PC 写入变化（PC2P4.TXT）的次数 */
    /* ---- WS-F 补充诊断 ---- */
    uint32_t op_counts[FILE_SERVICE_OP_COUNT]; /**< 每个 op 的请求计数 */
    uint32_t req_dropped;       /**< 请求队列满而立刻回 EBUSY 的请求数 */
    uint32_t reply_dropped;     /**< 响应入 TX 队列失败（主机侧会超时） */
    uint32_t unmounted;         /**< 因 SD 未挂载被拒的请求数 */
    uint32_t path_rejected;     /**< 路径校验失败（".."/超长/非法字符）的请求数 */
    uint32_t rx_ignored;        /**< 主机误发的响应类/未知 op，被忽略的条数 */
    uint32_t share_hb_writes;   /**< 心跳文件写入次数 */
    uint32_t share_pc2p4_len;   /**< 最近一次看到 PC2P4.TXT 的长度 */
    bool     sd_mounted;        /**< 采样时刻 sd_storage_is_mounted() */
    bool     started;           /**< 服务是否已启动 */
} file_service_stats_t;

/**
 * @brief 启动 FILE 服务（注册 webusb_proto 的 FILE handler + 起共享探针任务）。
 *
 * 幂等；SD 未挂载时也返回 ESP_OK（请求统一回 EBUSY，探针每 1 s 重试挂载状态）。
 * 必须在 `usb_vendor_start()` 之前调用（见文件头：caps 的 FILE 位）。
 *
 * @return ESP_OK；ESP_ERR_NO_MEM（队列/任务创建失败）
 */
esp_err_t file_service_start(void);

/** 服务是否已启动 */
bool file_service_is_started(void);

/**
 * @brief 读取统计快照（任意任务、NULL 安全）。
 *
 * 与 FILE 服务任务并发安全（内部自旋锁）；未启动时全部为 0。
 */
void file_service_get_stats(file_service_stats_t *out);

/**
 * @brief 服务根目录（= `sd_storage_mount_point()`，**永远非 NULL**）。
 *
 * 单独暴露是为了让验收工具/日志不必再写一遍 "/sdcard" 字面量。
 */
const char *file_service_root(void);

#ifdef __cplusplus
}
#endif
