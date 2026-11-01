// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议「设备侧服务」—— 设备侧 API（本组件自有，**非冻结**）
//
//   规约：docs/protocol.md（帧头/通道/flags/CRC/ZLP/流控/重连/§6.5 补充裁决）
//   契约：components/webusb_proto/include/wu_codec.h（**冻结，只读**）
//   实现：components/webusb_proto/webusb_proto.c
//   消费者：components/usb_vendor/（IF2 的 on_out / init 回调；IF1 视频只借 ep_write 计数）
//
// ⚠️ 来源说明：本头最初由 ws-webusb2 作为交接草稿写下（Lead 从未落盘它，也**不是**冻结物）。
//    task-3 确认由 ws-webusb 继续后，**ws-webusb 采纳并维护本头**（它是"设备侧服务"的对外面，
//    不是 I0 的一部分 —— I0 里只有 wu_codec.h 是冻结的）。改签名请连同 webusb_proto.c
//    与 components/usb_vendor/ 的调用点一起改。
//
// 分层：
//   wu_codec.c     纯 C 编解码（可用原生 gcc 编译，离线验收链）
//   webusb_proto.c 本文件实现的服务层：CTRL/FILE 可靠通道（W=8 + 累计 ACK + 200 ms 重传 ×5）、
//                  HELLO/HELLO_ACK 状态机、3 s link_down、LOG/TELEM 周期发送、
//                  诊断计数器（crcErrors / resyncBytes / zeroLenReads / linkDowns）
//
// ⚠️ 本文件是**设备侧**的 API，不是 I0 的一部分；I0 里没有冻结的 C API（只有 wu_codec.h）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wu_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 冻结值（引自 docs/protocol.md，实现里不得再出现字面量）
 * ========================================================================= */

/** 可靠通道滑动窗口（协议 §6.1：W = 8 帧） */
#define WEBUSB_PROTO_TX_WINDOW      8
/** 帧超时重传（协议 §6.1：200 ms） */
#define WEBUSB_PROTO_RETX_TIMEOUT_MS 200
/** 连续重传失败上限（协议 §6.1：5 次 ⇒ link_down） */
#define WEBUSB_PROTO_RETX_MAX       5
/** HELLO 周期（协议 §6.3：每 2 s 发一次直到 HELLO_ACK） */
#define WEBUSB_PROTO_HELLO_MS       2000
/** 心跳 PING 周期（协议 §5.4：1 s） */
#define WEBUSB_PROTO_PING_MS        1000
/** TELEM 周期（协议 §5.4：1 s） */
#define WEBUSB_PROTO_TELEM_MS       1000
/** LOG 周期（任务书：1 s） */
#define WEBUSB_PROTO_LOG_MS         1000
/** link_down 判据（协议 §6.3：3 s 没收到任何有效帧） */
#define WEBUSB_PROTO_LINK_TIMEOUT_MS 3000

/** WiFi 扫描条目：i8 rssi + u8 authmode + char ssid[33]（协议 §5.1） */
#define WU_WIFI_SSID_MAX            33
#define WU_WIFI_PASS_MAX            65
#define WU_WIFI_ENTRY_LEN           (1 + 1 + WU_WIFI_SSID_MAX)
/** 单条 WIFI_SCAN_RSP 最多带上表条目数（受 CTRL 单帧 1024 B 上限约束：4 + N*35 ≤ 1024） */
#define WU_WIFI_MAX_ENTRIES         24

/** WiFi 状态（WIFI_STATUS 的 arg，协议 §5.1） */
enum {
    WU_WIFI_DOWN = 0,
    WU_WIFI_CONNECTING = 1,
    WU_WIFI_UP = 2,
};

/** 一条扫描结果 */
typedef struct {
    int8_t rssi;
    uint8_t authmode;
    char ssid[WU_WIFI_SSID_MAX]; /**< NUL 结尾；线上仍按定长 33 B 发送 */
} wu_wifi_entry_t;

/* =========================================================================
 * 外部后端钩子
 * ========================================================================= */

/**
 * @brief WiFi 后端（WS-net 落地前为 NULL）。
 *
 * ⚠️ 这些回调在**协议服务任务**上下文里被调用，必须**快速返回**（不可长阻塞）。
 * ops 为 NULL 时：HELLO 的 caps 不声明 WIFI，WIFI_SCAN 回 `WU_ST_ENOTSUP`，
 * WIFI_CONNECT 不生效并回一条 `WIFI_STATUS = DOWN`。
 */
typedef struct {
    /** 触发一次扫描（结果由 webusb_proto_wifi_scan_result() 从任意任务回填） */
    esp_err_t (*scan_start)(void);
    /** 发起连接（ssid/pass 均为 NUL 结尾） */
    esp_err_t (*connect)(const char *ssid, const char *pass);
    /** 读当前状态：state = WU_WIFI_*，ipv4_be = 网络序 IPv4，mac[6]，rssi */
    void (*get_status)(uint8_t *state, uint32_t *ipv4_be, uint8_t mac[6], int8_t *rssi);
} webusb_proto_wifi_ops_t;

/** FILE 通道消息处理器（file_service.c 落地前保持 NULL ⇒ 统一回 ENOTSUP） */
typedef void (*webusb_proto_file_msg_cb_t)(const uint8_t *msg, size_t len, void *ctx);

/* =========================================================================
 * 诊断统计（协议 §6.4 要求 crcErrors/resyncBytes/zeroLenReads 可读）
 * ========================================================================= */

typedef struct {
    /* ---- 协议 §6.4 点名的三个计数器 ---- */
    uint32_t crc_errors;
    uint32_t resync_bytes;
    uint32_t zero_len_reads; /**< 设备侧口径：on_out 被调用但 OUT 已排空（ZLP 的对应事件） */
    /* ---- 其余诊断 ---- */
    uint32_t bad_header;
    uint32_t msg_aborted;
    uint32_t link_downs;
    uint32_t link_ups;
    uint32_t helloSent;      /**< 设备侧发出的 HELLO 帧数（周期性直到收到 HELLO_ACK） */
    uint32_t helloAckRecv;   /**< 收到的 HELLO_ACK 次数（= link_up 次数） */
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t retx;        /**< 重传次数 */
    uint32_t tx_dropped;  /**< 窗口/缓冲不足而丢弃的帧数 */
    uint32_t rx_dup;      /**< 重复 seq（幂等，不重复投递） */
    uint32_t rx_gap;      /**< seq 缺口（丢帧并催促重传） */
    uint32_t rx_unknown_ch;
    uint32_t rx_overflows; /**< RX 缓冲满而丢掉的字节数 */
    uint32_t ctrl_tx_msgs;
    uint32_t ctrl_rx_msgs;
    uint32_t ctrl_bad_msgs;
    uint32_t file_tx_msgs;
    uint32_t file_rx_msgs;
    uint32_t log_tx;
    uint32_t telem_tx;
    uint32_t in_bytes;  /**< 设备→主机：本服务 + usb_vendor 写进 IN 端点的字节数 */
    uint32_t out_bytes; /**< 主机→设备：从 OUT 端点读出的字节数 */
    bool link_up;
    uint32_t since_rx_ms; /**< 距最近一次收到有效帧的毫秒数 */
    uint32_t link_generation;
} webusb_proto_stats_t;

/* =========================================================================
 * 生命周期
 * ========================================================================= */

/**
 * @brief 启动协议服务（建缓冲/队列/任务）。幂等。
 *
 * 由 `usb_vendor_start()` 调用；必须在 `usb_composite_start()` 之前。
 * 服务任务在**未 mount / 未拿到端点号**时只做计时，不碰 USB。
 */
esp_err_t webusb_proto_start(void);

/** 服务是否已启动 */
bool webusb_proto_is_started(void);

/**
 * @brief 由 vendor 功能的 `init(alloc)` 回调告知 IF2 的端点号（含方向位 / 不含方向位）。
 *
 * 端点号一律来自 `usb_fn_alloc_t`，本服务**绝不**硬编码。
 */
void webusb_proto_set_endpoints(uint8_t ep_in, uint8_t ep_out);

/** 主机枚举/断开通知（false 时立即判 link_down 并清空状态） */
void webusb_proto_set_mounted(bool mounted);

/** 注册 WiFi 后端；NULL = 注销 */
void webusb_proto_set_wifi_ops(const webusb_proto_wifi_ops_t *ops);

/** 注册 FILE 通道处理器；NULL = 未实现（回 ENOTSUP） */
void webusb_proto_set_file_handler(webusb_proto_file_msg_cb_t cb, void *ctx);

/* =========================================================================
 * 数据面
 * ========================================================================= */

/**
 * @brief OUT 端点有数据可读（vendor 功能的 `on_out` 回调）。**不可阻塞**。
 *
 * 语义与 I1 契约一致：一次读干净直到 `usb_composite_ep_read()` 返回 0。
 * 读出的字节进 RX 环缓冲，真正的解析在服务任务里做（单线程，无锁）。
 * 调用时若第一次读就返回 0 ⇒ 计入 `zero_len_reads`（ZLP 的设备侧对应事件，协议 §4.1）。
 */
void webusb_proto_on_out(uint8_t ep_out_addr);

/**
 * @brief 写 IN 端点并计入 `in_bytes`（视频功能也走这里，保证 TELEM 的 usb_in_bytes 含视频）。
 * @return 实际接受的字节数（可能 < len，调用方负责重试）
 */
size_t webusb_proto_ep_write(uint8_t ep_in, const void *data, size_t len);

/* =========================================================================
 * 链路状态（usb_vendor 的视频通道用它做门控 / 序号复位）
 * ========================================================================= */

bool webusb_proto_is_link_up(void);

/**
 * @brief 链路「代」计数：每次 link_up 或 link_down 都会 +1。
 *
 * 视频发送任务靠它发现"设备的 TX 序号已被归 0"，从而把自己的 VIDEO seq 也归 0
 * （协议 §6.3：收到 HELLO_ACK 时把**自己所有 TX seq 归 0**）。
 */
uint32_t webusb_proto_link_generation(void);

/* =========================================================================
 * 发送入口（可从任意任务调用；内部走队列，不阻塞）
 * ========================================================================= */

/** 发一行 LOG（不可靠通道 3；超长按 WU_MAX_PAYLOAD 截断）。len = 0 表示用 strlen */
esp_err_t webusb_proto_send_log(const char *text, size_t len);

/** printf 版日志（一行 ≤ 118 B） */
int webusb_proto_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief 发一条 FILE 通道消息（8 B 子头 + body，见协议 §5.3）。可靠通道。
 *
 * 供将来的 `components/usb_vendor/file_service.c` 使用；本阶段没有实现者，
 * 收到 FILE 请求时统一回 `WU_ST_ENOTSUP`。
 */
esp_err_t webusb_proto_send_file_reply(uint8_t op, uint8_t status, uint16_t xid,
                                      uint32_t offset, const void *body, size_t len);

/** 回填一次扫描结果（任意任务；进 1 深队列，服务任务负责发 WIFI_SCAN_RSP） */
esp_err_t webusb_proto_wifi_scan_result(const wu_wifi_entry_t *entries, size_t count);

/** 主动发一条 WIFI_STATUS（例如 WIFI_CONNECT 之后） */
esp_err_t webusb_proto_wifi_status(uint8_t state, uint32_t ipv4_be, const uint8_t mac[6],
                                   int8_t rssi);

/** 立即安排一帧 TELEM（不可靠通道 4；实际由服务任务在下一轮发出） */
esp_err_t webusb_proto_send_telem(void);

/**
 * @brief 单次轮询（协议任务内部用）。
 *
 * 任务已在跑时它是 **no-op**；只有 `webusb_proto_start()` 未被调用（自检/宿主测试）时才真正跑一次。
 */
void webusb_proto_poll(void);

/* =========================================================================
 * 统计
 * ========================================================================= */

void webusb_proto_get_stats(webusb_proto_stats_t *out);

/** 组装 §5.4 的 36 B TELEM 体（也用作 STATS_RSP 的 body，口径见 .c 内注释） */
void webusb_proto_build_telem(uint8_t out[WU_TELEM_LEN]);

#ifdef __cplusplus
}
#endif
