// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议「设备侧服务」—— 实现
//
// 规约：docs/protocol.md（**冻结**；§6.5 是 2026-10 补充裁决）
// 契约：components/webusb_proto/include/wu_codec.h（**冻结**）
//       components/webusb_proto/include/webusb_proto.h（本组件自有；原为交接草稿，现由本人维护）
//
// 分层：
//   wu_codec.c     纯 C 编解码（可被原生 gcc 编译，离线验收链）
//   webusb_proto.c 本文件：可靠通道（W=8 + 累计 ACK + 200 ms 重传 ×5）、HELLO/HELLO_ACK、
//                  3 s link_down、CTRL 各 opcode、LOG/TELEM 周期、FILE 通道挂钩、诊断计数
//
// 数据面**只**经 `usb_composite_ep_write()` / `usb_composite_ep_read()`；端点号来自
// `usb_fn_alloc_t`（`webusb_proto_set_endpoints()`）。本文件**没有任何硬编码 IF/EP 号**。
//
// 关键实现决定（含 docs/protocol.md §6.5 的五条）：
//   ① 每个通道的 ACK 走它自己的通道（§6.5①）⇒ ACK 帧的 `ch` = 被确认帧的 `ch`；
//      载荷用 CTRL 的 4 字节子头（opcode 0x05），因为 FILE 的 8 字节头里没有 ACK 语义。
//      接收侧靠"**长度恰为 4 且首字节 = 0x05**"识别 ACK —— FILE 消息最短 8 字节，故无歧义。
//   ② ACK 帧吃**本通道自己的**下一个 seq（§6.5②），与数据帧共用一个计数器。
//   ③ ACK 帧 `flags = 0`（§6.5③），不入窗、不重传，避免 ACK 风暴。
//   ④ 引导帧（设备的 `HELLO`、主机的 `HELLO_ACK`、引导期 `PING`）是**无状态**帧
//      （seq=0/flags=0、不入窗口）。§6.5④ 只点名了主机 PING；本实现把同一条理由
//      （周期性 HELLO 会触发序号重置 ⇒ 占窗的引导帧会变成悬空未确认帧）一致地用在
//      HELLO/HELLO_ACK 上。**C 与 web/proto.js 两侧实现完全一致。**
//      ⇒ 推论：引导帧必须**绕开 seq 门**。否则"引导 PING(seq=0)"会把随后的
//        "HELLO_ACK(seq=0)"顶成重复帧，链路永远收敛不了（两侧都实现了这条豁免）。
//   ⑤ `resync_bytes` 口径 = 非法候选头 12 B 不计入（归 bad_header 事件计），其余垃圾计入
//      （见 wu_codec.c 与 wu_codec.h 的冻结文字）。
//
// ⚠️ 与 video_pipe（I2，WS-video）的关系：本文件只读 video_pipe 的统计（TELEM）与投递 UI 命令，
//    且是**强引用**（见下面 include 处的说明与 CMakeLists 的 REQUIRES）。
//    视频帧的实际发送在 components/usb_vendor/（不在这里）。

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "usb_composite.h"
#include "webusb_proto.h"
#include "wu_codec.h"

/* I2 帧源：**强依赖**（`CMakeLists.txt` 里 REQUIRES video_pipe）。
   2026-10 Lead 明确要求**不要**用 weak 引用容忍"组件未落地" —— probe-report §3.2 的教训
   （esp_tinyusb 的 MTP 强符号因无人引用而没被链接、弱默认胜出 → 全部 stall）说明
   弱符号会把"某组件没被链接"这类真实缺陷掩盖成"暂时没帧"。所以这里直接强引用。 */
#include "video_pipe.h"

static const char *TAG = "wu_proto";

/* =========================================================================
 * 规模常量
 * ========================================================================= */

#define WU_RX_RING_BYTES        4096    /* EP4 OUT 字节环（TinyUSB 任务写 / 协议任务读） */
#define WU_TX_QDEPTH            6       /* 任意任务 → 协议任务的出站消息队列 */
#define WU_TASK_STACK           8192
#define WU_TASK_PRIO            5
#define WU_POLL_MS              10
#define WU_EP_WRITE_WAIT_MS     100     /* 单帧写不完时的等待上限（只在协议任务里等） */
#define WU_EP_BACKPRESSURE      2048    /* IN 端点待发字节超过它就跳过本轮不可靠发送 */

#define WU_MSG_CTRL_MAX         2048
#define WU_MSG_FILE_MAX         4096
#define WU_MSG_LOG_MAX          1024
#define WU_MSG_TELEM_MAX        64
#define WU_MSG_VIDEO_MAX        1024

#define WU_RTX_CTRL             0
#define WU_RTX_FILE             1
#define WU_RTX_COUNT            2

#define WU_WIN                  WEBUSB_PROTO_TX_WINDOW   /* = 8（协议 §6.1） */

/* =========================================================================
 * 类型
 * ========================================================================= */

/** 出站消息记录（任意任务入队 → 协议任务发） */
typedef struct {
    uint8_t  ch;        /**< WU_CH_CTRL / WU_CH_FILE / WU_CH_LOG / WU_CH_TELEM */
    uint8_t  op;
    uint8_t  status;
    uint16_t arg;
    uint32_t offset;
    uint16_t len;
    uint8_t  body[WU_MAX_PAYLOAD];
} wu_tx_rec_t;

/** 可靠窗口里的一帧（重传要能原样重发，故存载荷 + 头字段） */
typedef struct {
    bool     used;
    uint16_t seq;
    uint16_t flags;
    uint16_t len;
    uint8_t  retries;
    uint32_t sent_ms;
    uint8_t  payload[WU_MAX_PAYLOAD];
} wu_tx_slot_t;

/** 一个可靠通道的发送侧（CTRL / FILE 各一个） */
typedef struct {
    uint8_t  ch;
    uint16_t next_seq;      /* 本通道本方向的下一个 seq（数据帧与 ACK 帧共用，§6.5②） */
    uint8_t  head;
    uint8_t  count;
    bool     dead;
    uint32_t last_progress_ms;
    wu_tx_slot_t slot[WU_WIN];
} wu_rtx_t;

/* =========================================================================
 * 静态状态
 * ========================================================================= */

static bool          s_started;
static TaskHandle_t  s_task;
static QueueHandle_t s_txq;
static StaticQueue_t s_txq_cb;
static wu_tx_rec_t   s_txq_storage[WU_TX_QDEPTH];

static uint8_t s_ep_in;          /* IF2 bulk IN（含 0x80） */
static uint8_t s_ep_out;         /* IF2 bulk OUT（不含方向位） */
static bool    s_eps_valid;
static bool    s_mounted;

static uint8_t      s_rx_ring[WU_RX_RING_BYTES];
static size_t       s_rx_head, s_rx_tail;
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;

static wu_decoder_t   s_dec;
static wu_assembler_t s_asm[WU_CH_MAX];
static uint8_t s_asm_buf_ctrl[WU_MSG_CTRL_MAX];
static uint8_t s_asm_buf_file[WU_MSG_FILE_MAX];
static uint8_t s_asm_buf_log[WU_MSG_LOG_MAX];
static uint8_t s_asm_buf_telem[WU_MSG_TELEM_MAX];
static uint8_t s_asm_buf_video[WU_MSG_VIDEO_MAX];

static uint16_t s_rx_expect[WU_CH_MAX];   /* 可靠通道的"下一个期望 seq" */
static uint16_t s_tx_seq[WU_CH_MAX];      /* 不可靠通道的 TX seq */
static uint16_t s_credit[WU_CH_MAX];      /* 收到的 CREDIT（记录用；本阶段不据此收敛发送） */
static wu_rtx_t s_rtx[WU_RTX_COUNT];

static webusb_proto_stats_t        s_stats;
static const webusb_proto_wifi_ops_t *s_wifi;
static webusb_proto_file_msg_cb_t  s_file_cb;
static void                       *s_file_ctx;

static bool     s_link_up;
static uint32_t s_link_gen;
static uint32_t s_last_rx_ms;
static uint32_t s_last_hello_ms;
static uint32_t s_last_ping_ms;
static uint32_t s_last_telem_ms;
static uint32_t s_last_log_ms;

/* 延迟动作：**绝不在解码回调里复位状态**（回调里 wu_decoder_t 正被遍历，复位 = use-after-reset） */
static volatile bool s_flag_link_up;
static volatile bool s_flag_link_down;

/* =========================================================================
 * 小工具
 * ========================================================================= */

static uint32_t wu_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** seq 环形序：a 在 b 之前（含相等） */
static bool wu_seq_le(uint16_t a, uint16_t b)
{
    return (uint16_t)(b - a) < 0x8000u;
}

static uint8_t *wu_asm_buf(uint8_t ch)
{
    switch (ch) {
    case WU_CH_CTRL:  return s_asm_buf_ctrl;
    case WU_CH_FILE:  return s_asm_buf_file;
    case WU_CH_LOG:   return s_asm_buf_log;
    case WU_CH_TELEM: return s_asm_buf_telem;
    default:          return s_asm_buf_video;
    }
}

static size_t wu_asm_cap(uint8_t ch)
{
    switch (ch) {
    case WU_CH_CTRL:  return sizeof(s_asm_buf_ctrl);
    case WU_CH_FILE:  return sizeof(s_asm_buf_file);
    case WU_CH_LOG:   return sizeof(s_asm_buf_log);
    case WU_CH_TELEM: return sizeof(s_asm_buf_telem);
    default:          return sizeof(s_asm_buf_video);
    }
}

static wu_rtx_t *wu_rtx_of(uint8_t ch)
{
    return (ch == WU_CH_FILE) ? &s_rtx[WU_RTX_FILE] : &s_rtx[WU_RTX_CTRL];
}

/* =========================================================================
 * RX 字节环（TinyUSB 任务写 / 协议任务读）
 * ========================================================================= */

static void wu_rx_push(const uint8_t *d, size_t n)
{
    size_t i;
    portENTER_CRITICAL(&s_rx_mux);
    for (i = 0; i < n; i++) {
        size_t next = (s_rx_head + 1u) % sizeof(s_rx_ring);
        if (next == s_rx_tail) {
            s_stats.rx_overflows += (uint32_t)(n - i);
            break;
        }
        s_rx_ring[s_rx_head] = d[i];
        s_rx_head = next;
    }
    portEXIT_CRITICAL(&s_rx_mux);
}

static size_t wu_rx_pop(uint8_t *out, size_t cap)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_rx_mux);
    while (n < cap && s_rx_tail != s_rx_head) {
        out[n++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) % sizeof(s_rx_ring);
    }
    portEXIT_CRITICAL(&s_rx_mux);
    return n;
}

static void wu_rx_reset(void)
{
    portENTER_CRITICAL(&s_rx_mux);
    s_rx_head = 0;
    s_rx_tail = 0;
    portEXIT_CRITICAL(&s_rx_mux);
}

/* =========================================================================
 * 端点写入
 * ========================================================================= */

size_t webusb_proto_ep_write(uint8_t ep_in, const void *data, size_t len)
{
    size_t w;
    /*
     * ⚠️ 这里**不能**只收本服务的控制 IN（s_ep_in）：视频功能（usb_vendor_video.c）
     * 也走这个函数写它自己的 EP3，目的是让 TELEM 的 usb_in_bytes 把视频算进去。
     * 2026-10-22 T2 真机实测：旧代码 `ep_in != s_ep_in` 直接把视频帧全丢了
     * （设备侧 Video frames 在涨，但 Vendor frames_sent=0、主机 0 帧）。
     * 端点合法性由 usb_composite_ep_write() 负责（未知端点返回 0）。
     */
    if (!s_eps_valid || data == NULL || len == 0) {
        return 0;
    }
    w = usb_composite_ep_write(ep_in, data, len);
    s_stats.in_bytes += (uint32_t)w;
    return w;
}

/**
 * @brief 把一整帧写进端点（可等待，只在**协议任务**里调用）。
 *
 * 帧必须整帧写入，否则流里会出现撕裂帧（主机靠 CRC 丢弃 + resync 兜底，重传会补回来）。
 * 等待上限 `WU_EP_WRITE_WAIT_MS`，超时返回实收字节数让调用方记账。
 */
static size_t wu_write_all(uint8_t ep, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    uint32_t t0 = wu_now_ms();

    while (sent < len) {
        size_t w = webusb_proto_ep_write(ep, p + sent, len - sent);
        if (w == 0) {
            if ((uint32_t)(wu_now_ms() - t0) >= WU_EP_WRITE_WAIT_MS) {
                break;
            }
            vTaskDelay(1);
            continue;
        }
        sent += w;
    }
    return sent;
}

/* =========================================================================
 * 发送：不可靠通道（LOG / TELEM）
 * ========================================================================= */

static void wu_send_unreliable(uint8_t ch, const uint8_t *payload, size_t len)
{
    size_t frames, off = 0, i;
    uint8_t frame[WU_MAX_FRAME];

    if (!s_eps_valid) {
        return;
    }
    /* §4.1-3 的性能要求：IN 端点积压很多时不要再塞过期遥测（本 API 没有 flush 能力，
       只能用"跳过本轮"近似）。正确性由帧头 + CRC 保证。 */
    if (usb_composite_ep_write_pending(s_ep_in) > WU_EP_BACKPRESSURE) {
        s_stats.tx_dropped++;
        return;
    }

    frames = (len == 0) ? 1 : ((len + WU_MAX_PAYLOAD - 1) / WU_MAX_PAYLOAD);
    for (i = 0; i < frames; i++) {
        size_t chunk = len - off;
        size_t flen = 0;
        uint16_t flags;
        uint16_t seq = s_tx_seq[ch]++;

        if (chunk > WU_MAX_PAYLOAD) {
            chunk = WU_MAX_PAYLOAD;
        }
        flags = (i + 1 < frames) ? WU_FLAG_MORE : 0;
        if (wu_encode(ch, flags, seq, (chunk != 0) ? (payload + off) : NULL, (uint16_t)chunk,
                      frame, sizeof(frame), &flen) != 0) {
            return;
        }
        s_stats.frames_tx++;
        if (wu_write_all(s_ep_in, frame, flen) != flen) {
            s_stats.tx_dropped++;
            return;   /* 不可靠通道：宁可丢也不阻塞（与 §5.2 的同一原则） */
        }
        off += chunk;
    }
    if (ch == WU_CH_LOG) {
        s_stats.log_tx++;
    } else if (ch == WU_CH_TELEM) {
        s_stats.telem_tx++;
    }
}

/* =========================================================================
 * 发送：可靠通道（CTRL / FILE）
 * ========================================================================= */

static void wu_rtx_reset(wu_rtx_t *r, uint8_t ch)
{
    size_t i;
    r->ch = ch;
    r->next_seq = 0;
    r->head = 0;
    r->count = 0;
    r->dead = false;
    r->last_progress_ms = wu_now_ms();
    for (i = 0; i < WU_WIN; i++) {
        r->slot[i].used = false;
    }
}

/** 把一条消息按 §4 分片、逐帧入窗并发出。false = 窗口不够（调用方稍后重试） */
static bool wu_rtx_send_msg(wu_rtx_t *r, const uint8_t *msg, size_t len)
{
    size_t frames = (len == 0) ? 1 : ((len + WU_MAX_PAYLOAD - 1) / WU_MAX_PAYLOAD);
    size_t off = 0, i;

    if (r->dead) {
        return false;
    }
    if ((size_t)r->count + frames > (size_t)WU_WIN) {
        return false;   /* 窗口满：等 ACK 再发（协议 §6.1） */
    }

    for (i = 0; i < frames; i++) {
        size_t chunk = len - off;
        uint8_t frame[WU_MAX_FRAME];
        size_t flen = 0;
        uint16_t seq = r->next_seq;
        uint16_t flags;
        wu_tx_slot_t *slot;

        if (chunk > WU_MAX_PAYLOAD) {
            chunk = WU_MAX_PAYLOAD;
        }
        flags = (uint16_t)(WU_FLAG_ACK | ((i + 1 < frames) ? WU_FLAG_MORE : 0));

        if (wu_encode(r->ch, flags, seq, (chunk != 0) ? (msg + off) : NULL, (uint16_t)chunk,
                      frame, sizeof(frame), &flen) != 0) {
            return false;
        }
        r->next_seq = (uint16_t)(r->next_seq + 1);

        /* 先入窗再写：写失败 / 撕裂由 200 ms 超时重传兜底 */
        slot = &r->slot[(r->head + r->count) % WU_WIN];
        slot->used = true;
        slot->seq = seq;
        slot->flags = flags;
        slot->len = (uint16_t)chunk;
        slot->retries = 0;
        slot->sent_ms = wu_now_ms();
        if (chunk != 0) {
            memcpy(slot->payload, msg + off, chunk);
        }
        r->count++;
        r->last_progress_ms = wu_now_ms();

        s_stats.frames_tx++;
        if (wu_write_all(s_ep_in, frame, flen) != flen) {
            s_stats.tx_dropped++;
        }
        off += chunk;
    }
    return true;
}

/** 累计 ACK：`next_expected` 之前的在途帧全部释放（§6.1） */
static void wu_rtx_on_ack(wu_rtx_t *r, uint16_t next_expected)
{
    uint16_t last_acked = (uint16_t)(next_expected - 1);
    while (r->count > 0) {
        wu_tx_slot_t *slot = &r->slot[r->head];
        if (!wu_seq_le(slot->seq, last_acked)) {
            break;
        }
        slot->used = false;
        r->head = (uint8_t)((r->head + 1u) % WU_WIN);
        r->count--;
    }
    r->last_progress_ms = wu_now_ms();
}

/** 200 ms 无进展 ⇒ 重传窗口内最老一帧（置 RETX）；连续 5 次失败 ⇒ 判死（§6.1） */
static void wu_rtx_tick(wu_rtx_t *r, uint32_t t)
{
    wu_tx_slot_t *slot;
    uint8_t frame[WU_MAX_FRAME];
    size_t flen = 0;

    if (r->dead || r->count == 0) {
        return;
    }
    if ((uint32_t)(t - r->last_progress_ms) < WEBUSB_PROTO_RETX_TIMEOUT_MS) {
        return;
    }

    slot = &r->slot[r->head];
    if (slot->retries >= WEBUSB_PROTO_RETX_MAX) {
        ESP_LOGW(TAG, "可靠通道 ch=%u 连续 %u 次重传失败 -> link_down（协议 §6.1）"
                 " ｜ 卡住的帧: seq=%u len=%u flags=0x%04X, 头部 retries=%u count=%u",
                 (unsigned)r->ch, (unsigned)slot->retries,
                 (unsigned)slot->seq, (unsigned)slot->len, (unsigned)slot->flags,
                 (unsigned)slot->retries, (unsigned)r->count);
        r->count = 0;
        r->head = 0;
        r->dead = true;
        s_flag_link_down = true;
        return;
    }

    slot->retries++;
    s_stats.retx++;
    r->last_progress_ms = t;
    if (wu_encode(r->ch, (uint16_t)(slot->flags | WU_FLAG_RETX), slot->seq,
                  (slot->len != 0) ? slot->payload : NULL, slot->len,
                  frame, sizeof(frame), &flen) == 0) {
        s_stats.frames_tx++;
        if (wu_write_all(s_ep_in, frame, flen) != flen) {
            s_stats.tx_dropped++;
        }
    }
}

/** 回一个 ACK 帧（§6.5① 同通道 / ② 吃本通道 seq / ③ flags=0、不入窗） */
static void wu_send_ack(uint8_t ch, uint16_t next_expected)
{
    uint8_t payload[WU_CTRL_HDR_LEN];
    uint8_t frame[WU_MAX_FRAME];
    size_t flen = 0;
    /*
     * ⚠️ 2026-10-31 真机修订（§6.5②）：ACK 帧**不消耗**通道 seq，接收方也不计入期望值。
     * 原因：ACK 帧本身不进重传窗口（丢失不补发）。旧实现让它吃掉一个 seq 后，
     * 一旦这条 ACK 在 USB 上丢了，接收方的"期望 seq"就**永久**停在旧值：
     * 之后所有数据帧都被判 gap 丢弃 → 发送方重传 5 次 → link_down →
     * 重连后同一条 ACK 再丢 → 无限循环（真机指纹：gap 单调增长、dup=0、retx 增长）。
     * 现在 ACK 帧用 seq=0 发送，双方都在 seq 门之前按"长度恰为 4 且首字节 0x05"识别并消费它。
     */
    uint16_t seq = 0;
    payload[0] = WU_OP_ACK;
    payload[1] = WU_ST_OK;
    wu_wr16(&payload[2], next_expected);

    if (wu_encode(ch, 0, seq, payload, (uint16_t)sizeof(payload), frame, sizeof(frame), &flen) == 0) {
        s_stats.frames_tx++;
        (void)wu_write_all(s_ep_in, frame, flen);
    }
}

/** 组一条 CTRL / FILE 消息并立即发（协议任务上下文） */
static esp_err_t wu_reliable_msg_now(uint8_t ch, uint8_t op, uint8_t status, uint16_t arg,
                                     uint32_t offset, const uint8_t *body, size_t blen)
{
    uint8_t buf[WU_MAX_PAYLOAD];
    size_t plen;

    if (ch == WU_CH_FILE) {
        if (blen > (size_t)(WU_MAX_PAYLOAD - WU_FILE_HDR_LEN)) {
            return ESP_ERR_INVALID_SIZE;
        }
        buf[0] = op;
        buf[1] = status;
        wu_wr16(&buf[2], arg);
        wu_wr32(&buf[4], offset);
        if (blen != 0 && body != NULL) {
            memcpy(&buf[WU_FILE_HDR_LEN], body, blen);
        }
        plen = (size_t)WU_FILE_HDR_LEN + blen;
    } else {
        if (blen > (size_t)(WU_MAX_PAYLOAD - WU_CTRL_HDR_LEN)) {
            return ESP_ERR_INVALID_SIZE;
        }
        buf[0] = op;
        buf[1] = status;
        wu_wr16(&buf[2], arg);
        if (blen != 0 && body != NULL) {
            memcpy(&buf[WU_CTRL_HDR_LEN], body, blen);
        }
        plen = (size_t)WU_CTRL_HDR_LEN + blen;
    }

    if (!wu_rtx_send_msg(wu_rtx_of(ch), buf, plen)) {
        s_stats.tx_dropped++;
        return ESP_ERR_NO_MEM;   /* 窗口满 / 缓冲不足：**不计**消息数，队首保留重试 */
    }
    if (ch == WU_CH_FILE) {
        s_stats.file_tx_msgs++;
    } else {
        s_stats.ctrl_tx_msgs++;
    }
    return ESP_OK;
}

/* =========================================================================
 * 引导 / 链路状态
 * ========================================================================= */

/** 无状态引导帧：seq=0、flags=0、不入窗口（§6.5④ 的一致扩展，见文件头 ④） */
static void wu_send_bootstrap(uint8_t op, uint16_t arg, const uint8_t *body, size_t blen)
{
    uint8_t payload[WU_CTRL_HDR_LEN + 4];
    uint8_t frame[WU_MAX_FRAME];
    size_t flen = 0;

    if (!s_eps_valid || blen > 4) {
        return;
    }
    payload[0] = op;
    payload[1] = WU_ST_OK;
    wu_wr16(&payload[2], arg);
    if (blen != 0 && body != NULL) {
        memcpy(&payload[WU_CTRL_HDR_LEN], body, blen);
    }
    if (wu_encode(WU_CH_CTRL, 0, 0, payload, (uint16_t)(WU_CTRL_HDR_LEN + blen),
                  frame, sizeof(frame), &flen) == 0) {
        s_stats.frames_tx++;
        (void)wu_write_all(s_ep_in, frame, flen);
    }
}

static uint32_t wu_caps(void)
{
    uint32_t caps = WU_CAP_LOG | WU_CAP_TELEM;
    if (s_file_cb != NULL) {
        caps |= WU_CAP_FILE;
    }
    if (s_wifi != NULL && s_wifi->scan_start != NULL) {
        caps |= WU_CAP_WIFI;
    }
    /* I2 是**强依赖**（构建图内必有）；这里声明的是"固件具备该能力"。
       注：批 3 之前 `video_pipe.c` 是 Lead 的占位实现（永不产帧），
       此时 caps 仍声明 VIDEO_MJPEG，但主机侧只会看到"无帧"（video_active=0）。 */
    caps |= WU_CAP_VIDEO_MJPEG;
    caps |= WU_CAP_UI_CMD;
    return caps;
}

static void wu_send_hello(void)
{
    uint8_t body[4];
    wu_wr32(body, wu_caps());
    wu_send_bootstrap(WU_OP_HELLO, WU_VERSION, body, sizeof(body));
    s_stats.helloSent++;
}

/** 复位链路状态：所有 TX seq / RX 期望 / 拼接态 / 发送队列（协议 §6.3） */
static void wu_reset_link_state(void)
{
    uint8_t ch;

    for (ch = 0; ch < WU_CH_MAX; ch++) {
        s_rx_expect[ch] = 0;
        s_tx_seq[ch] = 0;
        s_credit[ch] = 0;
        (void)wu_assembler_init(&s_asm[ch], wu_asm_buf(ch), wu_asm_cap(ch));
    }
    wu_rtx_reset(&s_rtx[WU_RTX_CTRL], WU_CH_CTRL);
    wu_rtx_reset(&s_rtx[WU_RTX_FILE], WU_CH_FILE);
    wu_decoder_init(&s_dec);
    wu_rx_reset();
    if (s_txq != NULL) {
        (void)xQueueReset(s_txq);
    }
}

static void wu_do_link_up(void)
{
    wu_reset_link_state();
    s_link_up = true;
    s_link_gen++;
    s_last_rx_ms = wu_now_ms();
    s_stats.link_ups++;
    s_stats.helloAckRecv++;
    ESP_LOGI(TAG, "link_up：收到 HELLO_ACK；已把本侧 TX seq / RX 期望 / 拼接态 / 发送队列归零（§6.3）");
}

static void wu_do_link_down(const char *why)
{
    if (s_link_up) {
        s_stats.link_downs++;
        ESP_LOGW(TAG, "link_down: %s（清空拼接状态与未确认窗口，回到等 HELLO_ACK，§6.3）", why);
    }
    wu_reset_link_state();
    s_link_up = false;
    s_link_gen++;
    s_last_hello_ms = 0;   /* 立刻重新开始发 HELLO */
}

void webusb_proto_set_mounted(bool mounted)
{
    s_mounted = mounted;
    if (!mounted) {
        wu_do_link_down("主机断开 / 未挂载");
    } else {
        s_last_hello_ms = 0;   /* 挂载后立刻发 HELLO */
        s_last_rx_ms = wu_now_ms();
    }
}

bool webusb_proto_is_link_up(void)
{
    return s_link_up;
}

uint32_t webusb_proto_link_generation(void)
{
    return s_link_gen;
}

/* =========================================================================
 * 接收：解码 → seq 门 → 重组 → 派发
 * ========================================================================= */

static void wu_handle_ctrl(const uint8_t *msg, size_t len);
static void wu_handle_file(const uint8_t *msg, size_t len);

static void wu_on_msg(const uint8_t *msg, size_t len, void *ctx)
{
    uint8_t ch = (uint8_t)(uintptr_t)ctx;

    if (ch == WU_CH_CTRL || ch == WU_CH_FILE) {
        /* §6.5① + 长度无歧义：可靠通道上"长度恰为 4 且首字节 = 0x05"就是 ACK 帧 */
        if (len == (size_t)WU_CTRL_HDR_LEN && msg[0] == WU_OP_ACK) {
            wu_rtx_on_ack(wu_rtx_of(ch), wu_rd16(&msg[2]));
            return;
        }
        if (ch == WU_CH_CTRL) {
            s_stats.ctrl_rx_msgs++;
            wu_handle_ctrl(msg, len);
        } else {
            s_stats.file_rx_msgs++;
            wu_handle_file(msg, len);
        }
    }
    /* LOG / TELEM 在协议 §1 的表里是单向上行（设备→主机）；主机发来的忽略 */
}

static bool wu_on_frame(const wu_frame_t *f, void *ctx)
{
    bool reliable;
    bool bootstrap;
    (void)ctx;

    s_stats.frames_rx++;

    /*
     * ⚠️ 2026-10-31 真机修正（"浏览器无画面 / 几百帧后卡死"的根因）：
     * `s_last_rx_ms` **不能**在帧一到就更新 —— §6.3 的判据是"3 s 没收到任何**有效帧**"。
     * 旧代码在 seq 门之前就刷新它，于是：主机侧序号错位（rx_expect 停在第 1 帧、
     * 设备 TX seq 已到 569）时，主机发的每一帧都被 seq 门丢弃，但**仍然刷新了超时计时器**
     * ⇒ 设备永远不判 link_down ⇒ 永远不发 HELLO ⇒ 主机永远等不到重同步
     * ⇒ 双方死锁在"设备 link_up、主机 link_down"，画面 0 帧。
     * 现在：只有**通过 seq 门（或引导帧）**的帧才算"有效帧"。
     */

    if (f->channel >= WU_CH_MAX) {
        s_stats.rx_unknown_ch++;
        return true;
    }

    reliable = (f->channel == WU_CH_CTRL || f->channel == WU_CH_FILE);
    /* 引导帧绕开 seq 门（§6.3 + §6.5④，见文件头 ④） */
    bootstrap = (f->channel == WU_CH_CTRL && f->length == WU_CTRL_HDR_LEN &&
                 f->payload != NULL && f->payload[0] == WU_OP_HELLO_ACK);

    /* ACK 帧不参与 seq 期望值（见 wu_send_ack 的修订说明）：先消费，再过 seq 门 */
    if (reliable && f->length == WU_CTRL_HDR_LEN && f->payload != NULL &&
        f->payload[0] == WU_OP_ACK) {
        wu_rtx_on_ack(wu_rtx_of(f->channel), wu_rd16(&f->payload[2]));
        s_last_rx_ms = wu_now_ms();
        return true;
    }

    if (reliable && !bootstrap) {
        uint16_t expect = s_rx_expect[f->channel];
        if (f->seq != expect && wu_seq_le(f->seq, (uint16_t)(expect - 1))) {
            /* 重复 seq：幂等 —— 再回一次 ACK，**不**重复投递（§6.1） */
            s_stats.rx_dup++;
            if ((f->flags & WU_FLAG_ACK) != 0) {
                wu_send_ack(f->channel, expect);
            }
            return true;
        }
        if (f->seq != expect) {
            /* 乱序：累计 ACK 停在期望值，不投递；发送方 200 ms 后重传最老帧 */
            s_stats.rx_gap++;
            if ((f->flags & WU_FLAG_ACK) != 0) {
                wu_send_ack(f->channel, expect);
            }
            return true;
        }
        s_rx_expect[f->channel] = (uint16_t)(expect + 1);
        if ((f->flags & WU_FLAG_ACK) != 0) {
            wu_send_ack(f->channel, s_rx_expect[f->channel]);
        }
    }

    /* 走到这里的帧才是"有效帧"（§6.3 的 link_down 判据只认这个时间戳） */
    s_last_rx_ms = wu_now_ms();

    return wu_assembler_push(&s_asm[f->channel], f, wu_on_msg, (void *)(uintptr_t)f->channel);
}

static void wu_drain_rx(void)
{
    uint8_t buf[512];
    size_t n;

    for (;;) {
        n = wu_rx_pop(buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        (void)wu_decoder_push(&s_dec, buf, n, wu_on_frame, NULL);
    }

    /* 解码统计回填（协议 §6.4 要求 crc_errors / resync_bytes 可读） */
    s_stats.crc_errors = s_dec.stats.crc_errors;
    s_stats.resync_bytes = s_dec.stats.resync_bytes;
    s_stats.bad_header = s_dec.stats.bad_header;

    /* 延迟动作：**必须**在解码回调之外做。
     * ⚠️ 2026-10-22 T2 真机修正（顺序见 bug 原因）：
     *   旧顺序是"先 link_up 再 link_down"。真机日志出现过同一毫秒里
     *     `link_up：收到 HELLO_ACK` 紧跟 `link_down: 3 s 未收到有效帧`
     *   原因是 s_flag_link_down 是**更早**一轮（主机不在时）重传耗尽置上的，
     *   却在新的 HELLO_ACK 之后才被消费 ⇒ 刚建立的链路被陈旧标志立刻掐死，
     *   主机侧于是永远看不到视频（设备 Video 侧在出帧，但链路一直 down）。
     *   正确语义：**link_down 先处理、link_up 后处理** ⇒ 本轮收到的 HELLO_ACK 生效。 */
    if (s_flag_link_down) {
        s_flag_link_down = false;
        wu_do_link_down("可靠通道重传耗尽");
    }
    if (s_flag_link_up) {
        s_flag_link_up = false;
        wu_do_link_up();
    }
}

/* =========================================================================
 * CTRL / FILE 语义
 * ========================================================================= */

static void wu_send_stats_rsp(void)
{
    uint8_t body[WU_TELEM_LEN];
    webusb_proto_build_telem(body);
    (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_STATS_RSP, WU_ST_OK, 0, 0, body, sizeof(body));
}

static void wu_handle_ctrl(const uint8_t *msg, size_t len)
{
    uint8_t op, status;
    uint16_t arg;
    const uint8_t *body;
    size_t blen;

    if (len < (size_t)WU_CTRL_HDR_LEN) {
        s_stats.ctrl_bad_msgs++;
        return;
    }
    op = msg[0];
    status = msg[1];
    arg = wu_rd16(&msg[2]);
    body = msg + WU_CTRL_HDR_LEN;
    blen = len - WU_CTRL_HDR_LEN;
    (void)status;

    switch (op) {
    case WU_OP_HELLO_ACK:
        /* 设备是序号主导方：收到 HELLO_ACK ⇒ 复位本侧序号并 link_up（§6.3）。
           延迟到解码回调之外执行（回调里改解码器状态会 use-after-reset）。 */
        s_flag_link_up = true;
        break;

    case WU_OP_PING:
        (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_PONG, WU_ST_OK, 0, 0, NULL, 0);
        break;

    case WU_OP_PONG:
    case WU_OP_ACK:   /* 已在上层按"长度 4 + 0x05"处理，这里只是兜底不报错 */
        break;

    case WU_OP_CREDIT:
        if (arg < WU_CH_MAX && blen >= 2) {
            s_credit[arg] = wu_rd16(body);
        }
        break;

    case WU_OP_STATS_REQ:
        wu_send_stats_rsp();
        break;

    case WU_OP_WIFI_SCAN:
        if (s_wifi != NULL && s_wifi->scan_start != NULL) {
            esp_err_t e = s_wifi->scan_start();
            if (e != ESP_OK) {
                (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_WIFI_SCAN_RSP, WU_ST_EBUSY, 0, 0, NULL, 0);
            }
        } else {
            (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_WIFI_SCAN_RSP, WU_ST_ENOTSUP, 0, 0, NULL, 0);
        }
        break;

    case WU_OP_WIFI_CONNECT: {
        /* body = char ssid[33] + char pass[65]（协议 §5.1） */
        char ssid[WU_WIFI_SSID_MAX];
        char pass[WU_WIFI_PASS_MAX];

        memset(ssid, 0, sizeof(ssid));
        memset(pass, 0, sizeof(pass));
        if (blen >= (size_t)WU_WIFI_SSID_MAX) {
            memcpy(ssid, body, sizeof(ssid) - 1);
            if (blen > (size_t)WU_WIFI_SSID_MAX) {
                size_t n = blen - WU_WIFI_SSID_MAX;
                if (n > sizeof(pass) - 1) {
                    n = sizeof(pass) - 1;
                }
                memcpy(pass, body + WU_WIFI_SSID_MAX, n);
            }
        }
        if (s_wifi != NULL && s_wifi->connect != NULL) {
            esp_err_t e = s_wifi->connect(ssid, pass);
            if (e != ESP_OK) {
                (void)webusb_proto_wifi_status(WU_WIFI_DOWN, 0, NULL, 0);
            }
        } else {
            (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_WIFI_STATUS, WU_ST_ENOTSUP, WU_WIFI_DOWN, 0, NULL, 0);
        }
        break;
    }

    case WU_OP_UI_CMD: {
        /* I2 强依赖：把网页发来的 key=value 串交给 UI 任务；占位实现会返回 NOT_SUPPORTED */
        esp_err_t e = video_pipe_post_ui_cmd((const char *)body, blen);
        if (e != ESP_OK) {
            (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_UI_CMD, WU_ST_ENOTSUP, arg, 0, NULL, 0);
        }
        break;
    }

    default:
        (void)wu_reliable_msg_now(WU_CH_CTRL, op, WU_ST_ENOTSUP, arg, 0, NULL, 0);
        break;
    }
}

static void wu_handle_file(const uint8_t *msg, size_t len)
{
    uint8_t op;
    uint16_t xid;
    uint32_t offset;

    if (len < (size_t)WU_FILE_HDR_LEN) {
        return;
    }
    op = msg[0];
    xid = wu_rd16(&msg[2]);
    offset = wu_rd32(&msg[4]);

    if (s_file_cb != NULL) {
        s_file_cb(msg, len, s_file_ctx);   /* 8 字节头 + xid 配对规则属 I0，语义交给文件服务 */
        return;
    }

    /* 没有文件服务：对请求帧统一回 ENOTSUP（**绝不静默吞请求**）。
       op 0..9 是请求/响应成对（REQ 偶数、RSP = REQ+1）；
       DELETE/MKDIR/RENAME(10/11/12) 没有独立响应码，按同 op 回执。 */
    if (op <= WU_FILE_CLOSE_REQ) {
        if ((op % 2) == 0) {
            (void)wu_reliable_msg_now(WU_CH_FILE, (uint8_t)(op + 1), WU_ST_ENOTSUP, xid, offset, NULL, 0);
        }
    } else {
        (void)wu_reliable_msg_now(WU_CH_FILE, op, WU_ST_ENOTSUP, xid, offset, NULL, 0);
    }
}

/* =========================================================================
 * TELEM（协议 §5.4，36 B 定长）
 * ========================================================================= */

void webusb_proto_build_telem(uint8_t out[WU_TELEM_LEN])
{
    uint32_t kbps = 0, dropped = 0, fps_x100 = 0;
    uint8_t  wifi_state = WU_WIFI_DOWN;
    uint32_t ipv4_be = 0;
    uint8_t  mac[6];
    int8_t   rssi = 0;
    uint8_t  flags = 0;

    memset(out, 0, WU_TELEM_LEN);
    memset(mac, 0, sizeof(mac));

    {
        video_pipe_stats_t vs;
        memset(&vs, 0, sizeof(vs));
        video_pipe_get_stats(&vs);
        kbps = vs.kbps;
        dropped = vs.frames_dropped;
        fps_x100 = vs.fps_actual_x100;
    }
    if (s_wifi != NULL && s_wifi->get_status != NULL) {
        s_wifi->get_status(&wifi_state, &ipv4_be, mac, &rssi);
    }
    (void)ipv4_be;  /* WIFI_STATUS 才带 IPv4；TELEM 的 §5.4 布局里没有它 */

    wu_wr32(out + 0, wu_now_ms());
    wu_wr32(out + 4, (uint32_t)esp_get_free_heap_size());
    wu_wr32(out + 8, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    wu_wr32(out + 12, s_stats.in_bytes);
    wu_wr32(out + 16, s_stats.out_bytes);
    wu_wr32(out + 20, kbps);
    wu_wr32(out + 24, dropped);
    wu_wr32(out + 28, fps_x100);
    out[32] = (uint8_t)rssi;
    if (wifi_state == WU_WIFI_UP) {
        flags |= 0x01;
    }
    /* bit1 = sd_mounted：本组件不掌握 SD 状态，恒 0（未验证项，见完成报告） */
    if (fps_x100 != 0) {
        flags |= 0x04;
    }
    out[33] = flags;
    wu_wr16(out + 34, 0);   /* reserved 必须为 0 */
}

/* =========================================================================
 * 出站队列（任意任务 → 协议任务）
 * ========================================================================= */

static esp_err_t wu_enqueue(const wu_tx_rec_t *rec)
{
    if (s_txq == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_txq, rec, 0) != pdTRUE) {
        s_stats.tx_dropped++;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void wu_send_telem_now(void)
{
    uint8_t telem[WU_TELEM_LEN];
    webusb_proto_build_telem(telem);
    wu_send_unreliable(WU_CH_TELEM, telem, sizeof(telem));
}

static void wu_drain_txq(void)
{
    wu_tx_rec_t rec;

    /* 用 peek + 成功后 receive：可靠消息在窗口满时**留在队首**等下一轮，
       既不会丢，也不会因为"重新入队到队尾"而打乱 CTRL/FILE 的消息顺序。 */
    while (xQueuePeek(s_txq, &rec, 0) == pdTRUE) {
        if (rec.ch == WU_CH_LOG) {
            wu_send_unreliable(WU_CH_LOG, rec.body, rec.len);
        } else if (rec.ch == WU_CH_TELEM) {
            wu_send_telem_now();
        } else {
            esp_err_t e = wu_reliable_msg_now(rec.ch, rec.op, rec.status, rec.arg,
                                              rec.offset, rec.body, rec.len);
            if (e != ESP_OK) {
                return;   /* 窗口满：本轮到此为止，队首这条下轮再试（协议 §6.1） */
            }
        }
        (void)xQueueReceive(s_txq, &rec, 0);
    }
}

/* =========================================================================
 * 周期任务
 * ========================================================================= */

static void wu_send_status_log_now(void)
{
    char line[224];
    webusb_proto_stats_t st;
    int n;
    /* 诊断用：主机侧最需要的是"设备认为的期望 seq"和"设备当前要发的 seq"，
     * 以及重传窗口队首卡住的帧 —— 这三者一对齐就能判断序号错位在哪一侧。 */
    wu_rtx_t *rc = &s_rtx[WU_RTX_CTRL];
    wu_tx_slot_t *head = &rc->slot[rc->head];
    unsigned stuckSeq = (rc->count > 0) ? (unsigned)head->seq : 0xFFFFu;
    unsigned stuckRetry = (rc->count > 0) ? (unsigned)head->retries : 0;

    webusb_proto_get_stats(&st);
    n = snprintf(line, sizeof(line),
                 "link=%s gen=%u rx=%ufrm/%umsgs crc=%u resync=%u dup=%u gap=%u retx=%u caps=0x%02x"
                 " rxexp=%u/%u txnext=%u stuck=%u/%u win=%u",
                 s_link_up ? "up" : "down", (unsigned)s_link_gen, (unsigned)st.frames_rx,
                 (unsigned)(st.ctrl_rx_msgs + st.file_rx_msgs), (unsigned)st.crc_errors,
                 (unsigned)st.resync_bytes, (unsigned)st.rx_dup, (unsigned)st.rx_gap,
                 (unsigned)st.retx, (unsigned)wu_caps(),
                 (unsigned)s_rx_expect[WU_CH_CTRL], (unsigned)s_rx_expect[WU_CH_FILE],
                 (unsigned)(rc->next_seq),
                 stuckSeq, stuckRetry, (unsigned)rc->count);
    if (n > 0) {
        wu_send_unreliable(WU_CH_LOG, (const uint8_t *)line, (size_t)n);
    }
}

static void wu_poll_timers(uint32_t t)
{
    if (!s_mounted || !s_eps_valid) {
        return;
    }

    /* §6.3：连续 3 s 没收到任何**有效帧** ⇒ link_down
     *
     * ⚠️ 2026-10-22 T2 真机修正：这里必须用**有符号**差值。
     *   `t` 是在 wu_task 循环开头取的，而 `wu_do_link_up()` 在本次 `wu_drain_rx()` 里
     *   用 `wu_now_ms()` 更新了 `s_last_rx_ms` —— 它可能比 `t` **晚几毫秒**。
     *   无符号比较 `(uint32_t)(t - s_last_rx_ms) >= 3000` 于是下溢成 ~4.29e9
     *   ⇒ 刚 link_up 立刻被判"3 s 没收到有效帧" ⇒ link_down。
     *   真机现象：`link_up` 与 `link_down` 同一毫秒出现，主机侧偶发 0 帧。 */
    if (s_link_up && (int32_t)(t - s_last_rx_ms) >= WEBUSB_PROTO_LINK_TIMEOUT_MS) {
        wu_do_link_down("3 s 未收到有效帧");
        return;
    }

    if (!s_link_up) {
        /* 设备是序号主导方：每 2 s 发 HELLO 直到收到 HELLO_ACK（§6.3） */
        if ((uint32_t)(t - s_last_hello_ms) >= WEBUSB_PROTO_HELLO_MS) {
            s_last_hello_ms = t;
            wu_send_hello();
        }
        /*
         * ⚠️ 2026-10-31 真机修正：**不再**在引导期发"无状态 PING(seq=0)"。
         * 引导帧 seq 恒为 0，而主机侧可靠通道的 seq 门只豁免 HELLO_ACK，会把这条
         * seq=0 的 PING 当成正常可靠帧接受、把 rx_expect 推到 1。随后设备 link_up
         * 从 seq=0 重新开始 ⇒ 主机 expect(1) 与设备 seq(0) 永久错位 ⇒ 设备收不到 ACK
         * ⇒ 重传 5 次 link_down ⇒ 循环（真机指纹：主机"期望 1，收到 569"、设备反复 retx_exhausted）。
         * 引导期只留 HELLO 即可，这样 link_up 后第一条可靠帧一定是 seq=0，两侧对齐。
         */
    } else if ((uint32_t)(t - s_last_ping_ms) >= WEBUSB_PROTO_PING_MS) {
        /* link_up 之后 PING 是普通可靠帧（§6.5④） */
        s_last_ping_ms = t;
        (void)wu_reliable_msg_now(WU_CH_CTRL, WU_OP_PING, WU_ST_OK, 0, 0, NULL, 0);
    }

    /* TELEM 只在上行有余量时发，且**绝不占用视频端点**（§5.4） */
    if ((uint32_t)(t - s_last_telem_ms) >= WEBUSB_PROTO_TELEM_MS) {
        s_last_telem_ms = t;
        wu_send_telem_now();
    }
    if ((uint32_t)(t - s_last_log_ms) >= WEBUSB_PROTO_LOG_MS) {
        s_last_log_ms = t;
        wu_send_status_log_now();
    }

    wu_rtx_tick(&s_rtx[WU_RTX_CTRL], t);
    wu_rtx_tick(&s_rtx[WU_RTX_FILE], t);
}

static void wu_task(void *arg)
{
    bool first = true;
    (void)arg;

    for (;;) {
        uint32_t t = wu_now_ms();
        bool mounted = usb_composite_is_mounted();

        if (first || mounted != s_mounted) {
            first = false;
            webusb_proto_set_mounted(mounted);
        }
        if (s_eps_valid) {
            wu_drain_rx();
            wu_drain_txq();
            wu_poll_timers(t);
        }
        vTaskDelay(pdMS_TO_TICKS(WU_POLL_MS));
    }
}

/* =========================================================================
 * 生命周期
 * ========================================================================= */

void webusb_proto_set_endpoints(uint8_t ep_in, uint8_t ep_out)
{
    s_ep_in = ep_in;
    s_ep_out = ep_out;
    s_eps_valid = (ep_in != 0 && ep_out != 0);
    ESP_LOGI(TAG, "数据面端点（来自 usb_fn_alloc_t）: IN=0x%02X OUT=0x%02X", ep_in, ep_out);
}

esp_err_t webusb_proto_start(void)
{
    uint8_t ch;

    if (s_started) {
        return ESP_OK;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    wu_decoder_init(&s_dec);
    for (ch = 0; ch < WU_CH_MAX; ch++) {
        (void)wu_assembler_init(&s_asm[ch], wu_asm_buf(ch), wu_asm_cap(ch));
    }
    wu_rtx_reset(&s_rtx[WU_RTX_CTRL], WU_CH_CTRL);
    wu_rtx_reset(&s_rtx[WU_RTX_FILE], WU_CH_FILE);
    s_last_hello_ms = 0;
    s_last_ping_ms = 0;
    s_last_telem_ms = 0;
    s_last_log_ms = 0;
    s_link_gen = 0;

    s_txq = xQueueCreateStatic(WU_TX_QDEPTH, sizeof(wu_tx_rec_t), (uint8_t *)s_txq_storage, &s_txq_cb);
    if (s_txq == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(wu_task, "wu_proto", WU_TASK_STACK, NULL, WU_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_txq);
        s_txq = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "协议服务已启动 | W=%d retx=%dms×%d hello=%dms link_timeout=%dms（§6.1/§6.3）",
             WU_WIN, WEBUSB_PROTO_RETX_TIMEOUT_MS, WEBUSB_PROTO_RETX_MAX,
             WEBUSB_PROTO_HELLO_MS, WEBUSB_PROTO_LINK_TIMEOUT_MS);
    return ESP_OK;
}

bool webusb_proto_is_started(void)
{
    return s_started;
}

void webusb_proto_set_wifi_ops(const webusb_proto_wifi_ops_t *ops)
{
    s_wifi = ops;
}

void webusb_proto_set_file_handler(webusb_proto_file_msg_cb_t cb, void *ctx)
{
    s_file_cb = cb;
    s_file_ctx = ctx;
}

void webusb_proto_on_out(uint8_t ep_out_addr)
{
    uint8_t tmp[512];
    size_t total = 0;
    int guard;

    if (!s_eps_valid || ep_out_addr != s_ep_out) {
        return;
    }
    /* I1 契约：必须一次读干净（读到返回 0），否则 RX FIFO 卡住。
       给一个轮数上限（64×512 = 32 KB > 环容量），避免底层 API 异常时死循环。 */
    for (guard = 0; guard < 64; guard++) {
        size_t n = usb_composite_ep_read(ep_out_addr, tmp, sizeof(tmp));
        if (n == 0) {
            break;
        }
        wu_rx_push(tmp, n);
        total += n;
    }
    s_stats.out_bytes += (uint32_t)total;
    if (total == 0) {
        /* 协议 §4.1：0 字节读是合法事件（ZLP）——不产生帧、不推进状态，只计数 */
        s_stats.zero_len_reads++;
    }
}

/* =========================================================================
 * 对外发送 API
 * ========================================================================= */

esp_err_t webusb_proto_send_file_reply(uint8_t op, uint8_t status, uint16_t xid,
                                       uint32_t offset, const void *body, size_t len)
{
    wu_tx_rec_t rec;

    if (len > (size_t)(WU_MAX_PAYLOAD - WU_FILE_HDR_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(&rec, 0, sizeof(rec));
    rec.ch = WU_CH_FILE;
    rec.op = op;
    rec.status = status;
    rec.arg = xid;
    rec.offset = offset;
    rec.len = (uint16_t)len;
    if (len != 0 && body != NULL) {
        memcpy(rec.body, body, len);
    }
    return wu_enqueue(&rec);
}

esp_err_t webusb_proto_send_log(const char *text, size_t len)
{
    wu_tx_rec_t rec;

    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        len = strlen(text);
    }
    if (len > WU_MAX_PAYLOAD) {
        len = WU_MAX_PAYLOAD;   /* LOG 是不可靠通道，超长直接截断，不值得为它多分帧 */
    }
    memset(&rec, 0, sizeof(rec));
    rec.ch = WU_CH_LOG;
    rec.len = (uint16_t)len;
    memcpy(rec.body, text, len);
    return wu_enqueue(&rec);
}

int webusb_proto_logf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return n;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    if (webusb_proto_send_log(line, (size_t)n) != ESP_OK) {
        return -1;
    }
    return n;
}

esp_err_t webusb_proto_send_telem(void)
{
    wu_tx_rec_t rec;

    memset(&rec, 0, sizeof(rec));
    rec.ch = WU_CH_TELEM;   /* 出队时走不可靠路径并按 36 B 组帧 */
    return wu_enqueue(&rec);
}

esp_err_t webusb_proto_wifi_scan_result(const wu_wifi_entry_t *entries, size_t count)
{
    uint8_t body[WU_WIFI_MAX_ENTRIES * WU_WIFI_ENTRY_LEN];
    size_t sent = 0;

    if (entries == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 一条 RSP 最多 WU_WIFI_MAX_ENTRIES 条（4 + 24×35 = 844 ≤ 1024）；多出来的分多条发 */
    while (sent < count) {
        size_t n = count - sent;
        wu_tx_rec_t rec;
        size_t i;

        if (n > WU_WIFI_MAX_ENTRIES) {
            n = WU_WIFI_MAX_ENTRIES;
        }
        memset(body, 0, sizeof(body));
        for (i = 0; i < n; i++) {
            uint8_t *e = &body[i * WU_WIFI_ENTRY_LEN];
            e[0] = (uint8_t)entries[sent + i].rssi;
            e[1] = entries[sent + i].authmode;
            memcpy(&e[2], entries[sent + i].ssid, WU_WIFI_SSID_MAX);
        }
        memset(&rec, 0, sizeof(rec));
        rec.ch = WU_CH_CTRL;
        rec.op = WU_OP_WIFI_SCAN_RSP;
        rec.arg = (uint16_t)n;
        rec.len = (uint16_t)(n * WU_WIFI_ENTRY_LEN);
        memcpy(rec.body, body, rec.len);
        {
            esp_err_t e = wu_enqueue(&rec);
            if (e != ESP_OK) {
                return e;
            }
        }
        sent += n;
    }
    if (count == 0) {
        wu_tx_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.ch = WU_CH_CTRL;
        rec.op = WU_OP_WIFI_SCAN_RSP;
        return wu_enqueue(&rec);
    }
    return ESP_OK;
}

esp_err_t webusb_proto_wifi_status(uint8_t state, uint32_t ipv4_be, const uint8_t mac[6], int8_t rssi)
{
    uint8_t body[4 + 6 + 1];
    wu_tx_rec_t rec;

    wu_wr32(body, ipv4_be);
    if (mac != NULL) {
        memcpy(&body[4], mac, 6);
    } else {
        memset(&body[4], 0, 6);
    }
    body[10] = (uint8_t)rssi;
    memset(&rec, 0, sizeof(rec));
    rec.ch = WU_CH_CTRL;
    rec.op = WU_OP_WIFI_STATUS;
    rec.arg = state;
    rec.len = (uint16_t)sizeof(body);
    memcpy(rec.body, body, sizeof(body));
    return wu_enqueue(&rec);
}

void webusb_proto_poll(void)
{
    if (s_started) {
        return;   /* 任务已在跑，外部无需驱动 */
    }
    wu_drain_rx();
    wu_drain_txq();
    wu_poll_timers(wu_now_ms());
}

void webusb_proto_get_stats(webusb_proto_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    s_stats.crc_errors = s_dec.stats.crc_errors;
    s_stats.resync_bytes = s_dec.stats.resync_bytes;
    s_stats.bad_header = s_dec.stats.bad_header;
    s_stats.msg_aborted = s_asm[WU_CH_CTRL].stats.msg_aborted + s_asm[WU_CH_FILE].stats.msg_aborted +
                          s_asm[WU_CH_LOG].stats.msg_aborted + s_asm[WU_CH_TELEM].stats.msg_aborted +
                          s_asm[WU_CH_VIDEO].stats.msg_aborted;
    s_stats.link_up = s_link_up;
    s_stats.link_generation = s_link_gen;
    s_stats.since_rx_ms = (uint32_t)(wu_now_ms() - s_last_rx_ms);
    *out = s_stats;
}
