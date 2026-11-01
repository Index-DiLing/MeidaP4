// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议编解码器（C 侧）—— **冻结**
//
// 规约：docs/protocol.md（帧头 12 字节 / 通道 / flags / CRC-16-CCITT-FALSE / resync）
// 生产者：Lead。消费者：components/webusb_proto/（设备侧）· web/proto.js（主机侧，须逐字节一致）
//
// ⚠️ 本文件与实现**刻意不依赖 ESP-IDF**（只用 <stdint.h>/<stddef.h>/<string.h>），
//    这样 `test/host/test_codec.c` 能用原生 gcc 编译运行，和 Node 里的 proto.js 做**逐字节**对照。
//    任何 IDF 依赖都会破坏这条离线验收链 —— 不要往这里加 `esp_err.h`。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 常量（协议冻结值） ============================ */

#define WU_MAGIC0            0x4D  /* 'M' */
#define WU_MAGIC1            0x50  /* 'P' */
#define WU_MAGIC_LE          0x504D /* 小端读出的 u16 */
#define WU_VERSION           0x01
#define WU_HDR_LEN           12
#define WU_MAX_PAYLOAD       1024
#define WU_MAX_FRAME         (WU_HDR_LEN + WU_MAX_PAYLOAD)

/** 通道号 */
enum {
    WU_CH_CTRL = 0,
    WU_CH_VIDEO = 1,
    WU_CH_FILE = 2,
    WU_CH_LOG = 3,
    WU_CH_TELEM = 4,
    WU_CH_MAX = 5,
};

/** flags 位 */
enum {
    WU_FLAG_MORE = 0x0001,
    WU_FLAG_ACK = 0x0002,
    WU_FLAG_RETX = 0x0004,
    WU_FLAG_ABORT = 0x0008,
};

/** CTRL opcode */
enum {
    WU_OP_HELLO = 0x01,
    WU_OP_HELLO_ACK = 0x02,
    WU_OP_PING = 0x03,
    WU_OP_PONG = 0x04,
    WU_OP_ACK = 0x05,
    WU_OP_CREDIT = 0x06,
    WU_OP_WIFI_SCAN = 0x10,
    WU_OP_WIFI_SCAN_RSP = 0x11,
    WU_OP_WIFI_CONNECT = 0x12,
    WU_OP_WIFI_STATUS = 0x13,
    WU_OP_UI_CMD = 0x20,
    WU_OP_STATS_REQ = 0x40,
    WU_OP_STATS_RSP = 0x41,
};

/** CTRL status */
enum {
    WU_ST_OK = 0x00,
    WU_ST_EINVAL = 0x01,
    WU_ST_ENOENT = 0x02,
    WU_ST_EIO = 0x03,
    WU_ST_ENOSPC = 0x04,
    WU_ST_EBUSY = 0x05,
    WU_ST_ENOTSUP = 0x06,
    WU_ST_EUNKNOWN = 0x7F,
};

/** 能力位（HELLO 的 body） */
enum {
    WU_CAP_VIDEO_MJPEG = 1u << 0,
    WU_CAP_VIDEO_H264 = 1u << 1,
    WU_CAP_FILE = 1u << 2,
    WU_CAP_LOG = 1u << 3,
    WU_CAP_TELEM = 1u << 4,
    WU_CAP_WIFI = 1u << 5,
    WU_CAP_UI_CMD = 1u << 6,
};

/** VIDEO 消息 payload 里的固定 16 字节头 */
#define WU_VIDEO_HDR_LEN     16
/** TELEM 定长 payload */
#define WU_TELEM_LEN         36
/** FILE 消息 payload 里的固定 8 字节头 */
#define WU_FILE_HDR_LEN      8
/** CTRL 消息 payload 里的固定 4 字节头 */
#define WU_CTRL_HDR_LEN      4

/** FILE opcode */
enum {
    WU_FILE_LIST_REQ = 0,
    WU_FILE_LIST_RSP = 1,
    WU_FILE_OPEN_REQ = 2,
    WU_FILE_OPEN_RSP = 3,
    WU_FILE_READ_REQ = 4,
    WU_FILE_READ_RSP = 5,
    WU_FILE_WRITE_REQ = 6,
    WU_FILE_WRITE_RSP = 7,
    WU_FILE_CLOSE_REQ = 8,
    WU_FILE_CLOSE_RSP = 9,
    WU_FILE_DELETE = 10,
    WU_FILE_MKDIR = 11,
    WU_FILE_RENAME = 12,
};

/* ============================ 帧 / 编解码 ============================ */

/** 解出的一帧（payload 指向解码器内部缓冲，仅在下一次 push 前有效） */
typedef struct {
    uint8_t channel;
    uint16_t flags;
    uint16_t seq;
    uint16_t length;
    const uint8_t *payload;
} wu_frame_t;

/** CRC-16/CCITT-FALSE：poly=0x1021 init=0xFFFF refin=false refout=false xorout=0 */
uint16_t wu_crc16(const uint8_t *data, size_t len);

/** CRC，覆盖头部前 10 字节 + payload（即协议 §2 的 crc16 字段口径） */
uint16_t wu_frame_crc(const uint8_t *hdr_first10, const uint8_t *payload, size_t payload_len);

/**
 * @brief 编码一帧。
 *
 * @param payload  可为 NULL（当 length == 0）
 * @param out_len  成功时写入 12 + length
 * @return 0 成功；-1 参数非法；-2 缓冲不足
 */
int wu_encode(uint8_t channel, uint16_t flags, uint16_t seq,
              const uint8_t *payload, uint16_t length,
              uint8_t *out, size_t cap, size_t *out_len);

/** 解码统计（诊断用，协议 §6.4 要求可读） */
typedef struct {
    uint32_t frames;        /**< 成功解出的帧数 */
    uint32_t crc_errors;    /**< CRC 校验失败的帧数（**事件计数**） */
    uint32_t bad_header;    /**< magic/version/length 非法的候选帧数（**事件计数**） */
    /**
     * **纯粹为"找下一个候选帧头"而跳过的字节数**（**字节计数**，与上面两个事件计数正交）。
     *
     * ⚠️ 2026-10 裁决（Lead；ws-webui 报冲突 → ws-webusb2 提请裁决）：
     * **非法候选帧头自身的 12 字节不计入**（那一段由 `bad_header` 以**事件**计），
     * 只计入它之后到下一个 `4D 50` 之间被跳过的垃圾字节。
     *
     * 这样每个被丢弃的字节都有归属：要么属于某个候选头（`bad_header` 记事件），
     * 要么属于候选头之间的垃圾（`resync_bytes` 记字节）。两者互补，不重叠、不留空档。
     *
     * 实测基准（`tools/proto_vectors.json` 的 `stream/length1025_illegal`）：
     * `bad_header=1`、`resync_bytes=32`；且该值与**喂入粒度无关**
     * （整块喂 32 = 逐字节喂 32 —— 这是一条必须保持的不变量）。
     *
     * 注：Lead 最初裁决时曾口头估过"候选头也计入、约 44~53"，那个估计**不作为冻结值**；
     * 冻结的是上面这条"候选头不计、粒度不变"的口径。
     */
    uint32_t resync_bytes;
    uint32_t truncated;     /**< 处于"半帧"状态的次数（仅统计，不是错误） */
} wu_decoder_stats_t;

/** 流式解码器（无动态内存；结构体可静态分配，约 1 KB） */
typedef struct {
    uint8_t buf[WU_MAX_FRAME];
    size_t len;
    wu_decoder_stats_t stats;
} wu_decoder_t;

void wu_decoder_init(wu_decoder_t *d);

/** 一帧解出时的回调。返回 false 表示调用方要求停止本次 push 的后续处理。 */
typedef bool (*wu_frame_cb_t)(const wu_frame_t *frame, void *ctx);

/**
 * @brief 喂入任意字节，解出尽可能多的帧。
 *
 * 语义（协议 §6.4）：
 *  - 内部缓冲未满时先累积；满则尝试解析；
 *  - 头部非法（magic/version/length）⇒ 从缓冲第 2 字节起重新扫 magic，`resync_bytes` 计数；
 *  - CRC 失败 ⇒ 丢帧，`crc_errors++`，**并从该帧之后继续**（不是从帧内重扫，避免把 payload 当帧头）；
 *  - **0 字节输入是合法 no-op**（协议 §4.1：ZLP 不产生帧、不推进状态）。
 *
 * @return 成功解出的帧数
 */
int wu_decoder_push(wu_decoder_t *d, const uint8_t *data, size_t len,
                    wu_frame_cb_t cb, void *ctx);

/** 便捷封装：读一个小端 u16 / u32（不检查长度，调用方自行保证） */
static inline uint16_t wu_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t wu_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wu_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wu_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ============================ 消息分片器 ============================ */

/** 分片发送器状态（发送方一通道一个） */
typedef struct {
    uint8_t channel;
    uint16_t seq;       /**< 下一个要用的 seq */
    bool in_message;    /**< 是否正在发一条多分片消息 */
} wu_fragmenter_t;

void wu_fragmenter_init(wu_fragmenter_t *f, uint8_t channel, uint16_t seq0);

/**
 * @brief 把一段消息拆成帧，逐帧回调交给调用方发送。
 *
 * 调用方通过 `emit` 返回 false 表示"发不下了"（端点忙）：
 *  - 若发生在消息中途，`wu_fragmenter_abort()` 语义生效（消息被放弃，`seq` 不回退）；
 *  - 返回本次已成功交给 emit 的帧数。
 *
 * @param msg       完整消息
 * @param msg_len   消息长度
 * @param want_ack  可靠通道传 true（每帧置 WU_FLAG_ACK）
 * @param emit      emit(frame_bytes, frame_len, ctx) -> false 表示停止
 */
typedef bool (*wu_emit_cb_t)(const uint8_t *frame, size_t len, void *ctx);

int wu_fragmenter_send(wu_fragmenter_t *f, const uint8_t *msg, size_t msg_len,
                       bool want_ack, wu_emit_cb_t emit, void *ctx);

/** 组装统计 */
typedef struct {
    uint32_t msg_ok;
    uint32_t msg_aborted;   /**< 上一条没拼完就来了新消息，或收到 ABORT */
    uint32_t resets;
} wu_assembler_stats_t;

/** 消息重组器（接收方一通道一个；缓冲由调用方提供） */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool in_message;
    uint16_t next_seq;      /**< 期望的下一个 seq；仅诊断用 */
    bool seq_valid;
    wu_assembler_stats_t stats;
} wu_assembler_t;

/** @return 0 成功；-1 参数非法 */
int wu_assembler_init(wu_assembler_t *a, uint8_t *buf, size_t cap);

/** 消息拼装完成时的回调 */
typedef void (*wu_msg_cb_t)(const uint8_t *msg, size_t len, void *ctx);

/**
 * @brief 喂入一帧（由解码器回调里调用）。
 *
 * @return true 表示本帧被接受；false 表示被丢弃（乱序/复位/ABORT）
 */
bool wu_assembler_push(wu_assembler_t *a, const wu_frame_t *frame, wu_msg_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
