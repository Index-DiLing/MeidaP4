// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议编解码器（C 侧）—— 实现
//
// 规约：docs/protocol.md（帧头 12 字节小端 / 通道 / flags / CRC-16-CCITT-FALSE / resync）
// 契约：components/webusb_proto/include/wu_codec.h（**冻结，只读**）
//
// ⚠️⚠️ 本文件**禁止** include 任何 ESP-IDF 头文件（只能用 <stdint.h>/<stddef.h>/<string.h>）。
//     `test/host/test_codec.c` 用**原生 gcc** 编译本文件并与 `web/proto.js` 做逐字节对照；
//     任何 IDF 依赖都会打断这条离线验收链（见 wu_codec.h 顶部注释与 probe-contract §3）。
//
// 行为口径（冻结在 docs/protocol.md §6.4，两处 resync 语义在本文件里逐行注明）：
//   * magic/version/length 非法 ⇒ 从"下一个字节"起重新扫 `4D 50`；命中的这一帧计入 bad_header，
//     为重新同步丢弃的字节**不计入** resync_bytes（wu_codec.h 对该字段的原文定义）；
//   * "不在候选帧头位置上"的垃圾字节 ⇒ 计入 resync_bytes；
//   * CRC 失败 ⇒ 丢该帧（crc_errors++）并从**该帧之后**继续（绝不从帧内重扫：
//     payload 里完全可能出现 `4D 50`，从帧内重扫会把数据当帧头）；
//   * 0 字节输入是合法 no-op（协议 §4.1：ZLP 不产生帧、不推进任何状态）。
//
// 所有计数器对"一次喂 1 字节"与"一次喂整段"**结果相同**（frames/crc_errors/resync_bytes/
// bad_header 都只依赖字节流本身）。唯一例外是 `truncated`：它按定义就是"一次 push 结束时
// 处于半帧状态的次数"，因此与喂入粒度有关（wu_codec.h 已注明它"仅统计，不是错误"）。

#include "wu_codec.h"

#include <string.h>

/* ============================ CRC ============================ */

uint16_t wu_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF; /* init = 0xFFFF, refin/refout = false, xorout = 0 */
    size_t i;
    int b;

    if (data == NULL) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t wu_frame_crc(const uint8_t *hdr_first10, const uint8_t *payload, size_t payload_len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;

    /* 先喂 hdr[0..10)，再喂 payload —— 与协议 §2 的 crc16 字段口径逐字节一致 */
    if (hdr_first10 == NULL) {
        return crc;
    }
    for (i = 0; i < 10; i++) {
        crc ^= (uint16_t)((uint16_t)hdr_first10[i] << 8);
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    if (payload != NULL) {
        for (i = 0; i < payload_len; i++) {
            crc ^= (uint16_t)((uint16_t)payload[i] << 8);
            for (b = 0; b < 8; b++) {
                crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ============================ 编码 ============================ */

int wu_encode(uint8_t channel, uint16_t flags, uint16_t seq,
              const uint8_t *payload, uint16_t length,
              uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t hdr[WU_HDR_LEN];
    uint16_t crc;
    size_t total;

    if (out == NULL) {
        return -1;
    }
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (length > WU_MAX_PAYLOAD) {
        return -1;
    }
    if (payload == NULL && length != 0) {
        return -1;
    }
    total = (size_t)WU_HDR_LEN + (size_t)length;
    if (cap < total) {
        return -2;
    }

    hdr[0] = WU_MAGIC0;
    hdr[1] = WU_MAGIC1;
    hdr[2] = WU_VERSION;
    hdr[3] = channel;
    wu_wr16(&hdr[4], flags);
    wu_wr16(&hdr[6], length);
    wu_wr16(&hdr[8], seq);
    crc = wu_frame_crc(hdr, payload, length);
    wu_wr16(&hdr[10], crc);

    memcpy(out, hdr, WU_HDR_LEN);
    if (length != 0) {
        memcpy(out + WU_HDR_LEN, payload, length);
    }
    if (out_len != NULL) {
        *out_len = total;
    }
    return 0;
}

/* ============================ 解码 ============================ */

void wu_decoder_init(wu_decoder_t *d)
{
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof(*d));
}

/** 在 [from, avail) 里找下一个 `4D 50`；找不到返回规则见下 */
static size_t wu__find_magic(const uint8_t *p, size_t from, size_t avail)
{
    /* 需要 p[k] 与 p[k+1] 都在缓冲内才算"找到" */
    size_t k;
    for (k = from; k + 1 < avail; k++) {
        if (p[k] == WU_MAGIC0 && p[k + 1] == WU_MAGIC1) {
            return k;
        }
    }
    return avail; /* 未找到 */
}

/**
 * @brief 丢掉缓冲区前缀 `n` 字节，并为 resync 记账。
 *
 * `n` 必须 <= d->len。调用方用 `count_as_resync` 区分两种丢弃：
 *  - true  ：这是"候选帧头位置之外"的垃圾（计入 resync_bytes）；
 *  - false ：这是刚被判定非法的候选帧头本身（计入 bad_header，字节不计入 resync_bytes）。
 */
static void wu__consume(wu_decoder_t *d, size_t n, bool count_as_resync)
{
    if (n > d->len) {
        n = d->len;
    }
    if (n == 0) {
        return;
    }
    if (count_as_resync) {
        d->stats.resync_bytes += (uint32_t)n;
    }
    d->len -= n;
    if (d->len != 0) {
        memmove(d->buf, d->buf + n, d->len);
    }
}

int wu_decoder_push(wu_decoder_t *d, const uint8_t *data, size_t len,
                    wu_frame_cb_t cb, void *ctx)
{
    int frames = 0;
    size_t pos = 0;

    if (d == NULL) {
        return 0;
    }
    /* 协议 §4.1：0 字节输入是合法 no-op —— 不产生帧、不推进任何状态 */
    if (data == NULL || len == 0) {
        return 0;
    }

    for (;;) {
        size_t space = sizeof(d->buf) - d->len;
        size_t chunk = len - pos;

        if (space != 0 && chunk != 0) {
            if (chunk > space) {
                chunk = space;
            }
            memcpy(d->buf + d->len, data + pos, chunk);
            d->len += chunk;
            pos += chunk;
        }

        /* ---- 解析缓冲里尽可能多的帧 ---- */
        for (;;) {
            size_t avail = d->len;
            const uint8_t *p = d->buf;
            uint8_t version;
            uint16_t length, crc_calc, crc_wire;
            size_t k, need;
            wu_frame_t frame;
            bool keep_going;

            if (avail < 2) {
                /* 不足一个 magic：可能攒着 1 个 'M'，等下一批字节 */
                if (avail == 1 && p[0] != WU_MAGIC0) {
                    wu__consume(d, 1, true); /* 不是 magic 前缀，直接扔 */
                    continue;
                }
                d->stats.truncated++;
                break;
            }

            if (p[0] != WU_MAGIC0 || p[1] != WU_MAGIC1) {
                /* 不在候选帧头位置：重新扫 magic（协议 §6.4） */
                k = wu__find_magic(p, 1, avail);
                if (k >= avail) {
                    /* 没找到：保留末尾可能是 magic 前缀的那 1 字节，其余全是垃圾 */
                    size_t drop = (p[avail - 1] == WU_MAGIC0) ? (avail - 1) : avail;
                    wu__consume(d, drop, true);
                    d->stats.truncated++;
                    break;
                }
                wu__consume(d, k, true);
                continue;
            }

            if (avail < WU_HDR_LEN) {
                d->stats.truncated++;
                break; /* 帧头不完整，等下一批 */
            }

            version = p[2];
            length = wu_rd16(&p[6]);
            if (version != WU_VERSION || length > WU_MAX_PAYLOAD) {
                /*
                 * 候选帧头非法 ⇒ 从下一个字节起重新扫 magic。
                 *
                 * resync_bytes 必须**只依赖字节流本身、不依赖喂入粒度**（verifier 的
                 * 不变量断言），所以把被跳过的区域显式拆成两段：
                 *   ① 候选帧头本身（最多 12 B）—— 归 bad_header，**不计** resync_bytes
                 *      （wu_codec.h 对该字段的定义原文）；
                 *   ② 其余被跳过的字节 —— 位于"候选帧头位置之外"，属于垃圾，计入 resync_bytes。
                 * 若整段一起丢，逐字节喂入时 ①② 的边界会落在不同 push 里 → 计数分裂
                 * （整块喂得 0、逐字节喂得 32，2026 由 verifier 的 679 条断言抓到）。
                 */
                size_t skip, uncounted;

                d->stats.bad_header++;
                k = wu__find_magic(p, 1, avail);
                if (k >= avail) {
                    skip = (p[avail - 1] == WU_MAGIC0) ? (avail - 1) : avail;
                } else {
                    skip = k;
                }
                uncounted = (skip < (size_t)WU_HDR_LEN) ? skip : (size_t)WU_HDR_LEN;
                wu__consume(d, uncounted, false);        /* 候选头：已计 bad_header */
                wu__consume(d, skip - uncounted, true);  /* 中间垃圾：计 resync_bytes */
                if (k >= avail) {
                    d->stats.truncated++;
                    break;
                }
                continue;
            }

            need = (size_t)WU_HDR_LEN + (size_t)length;
            if (avail < need) {
                d->stats.truncated++;
                break; /* payload 还没到齐 */
            }

            crc_wire = wu_rd16(&p[10]);
            crc_calc = wu_frame_crc(p, p + WU_HDR_LEN, length);
            if (crc_wire != crc_calc) {
                /* CRC 失败 ⇒ 丢该帧并从该帧之后继续（绝不从帧内重扫） */
                d->stats.crc_errors++;
                wu__consume(d, need, false);
                continue;
            }

            frame.channel = p[3];
            frame.flags = wu_rd16(&p[4]);
            frame.seq = wu_rd16(&p[8]);
            frame.length = length;
            frame.payload = p + WU_HDR_LEN;

            keep_going = (cb != NULL) ? cb(&frame, ctx) : true;
            d->stats.frames++;
            frames++;
            wu__consume(d, need, false);
            if (!keep_going) {
                /* 调用方要求停止本次 push：剩余字节留在缓冲里 */
                return frames;
            }
        }

        if (pos >= len) {
            break;
        }
        if (d->len == sizeof(d->buf)) {
            /* 防御：缓冲已满且解析无进展（理论上不可达：合法帧最大 == 缓冲容量）。
               丢 1 字节保证前进，绝不在这里死循环。 */
            wu__consume(d, 1, true);
        }
    }

    return frames;
}

/* ============================ 消息分片器 ============================ */

void wu_fragmenter_init(wu_fragmenter_t *f, uint8_t channel, uint16_t seq0)
{
    if (f == NULL) {
        return;
    }
    f->channel = channel;
    f->seq = seq0;
    f->in_message = false;
}

int wu_fragmenter_send(wu_fragmenter_t *f, const uint8_t *msg, size_t msg_len,
                       bool want_ack, wu_emit_cb_t emit, void *ctx)
{
    size_t off = 0;
    int sent = 0;
    size_t total_frames;
    uint8_t buf[WU_MAX_FRAME];

    if (f == NULL || emit == NULL) {
        return 0;
    }
    if (msg == NULL && msg_len != 0) {
        return 0;
    }

    /* 0 字节消息 = 1 帧（length=0, MORE=0），与协议 §4 "消息 = 1 个或多个帧" 一致 */
    total_frames = (msg_len == 0) ? 1 : ((msg_len + WU_MAX_PAYLOAD - 1) / WU_MAX_PAYLOAD);

    for (size_t i = 0; i < total_frames; i++) {
        size_t chunk = msg_len - off;
        uint16_t flags = 0;
        size_t flen = 0;
        int rc;

        if (chunk > WU_MAX_PAYLOAD) {
            chunk = WU_MAX_PAYLOAD;
        }
        if (want_ack) {
            flags |= WU_FLAG_ACK;
        }
        if (i + 1 < total_frames) {
            flags |= WU_FLAG_MORE;
        }

        rc = wu_encode(f->channel, flags, f->seq, (chunk != 0) ? (msg + off) : NULL,
                       (uint16_t)chunk, buf, sizeof(buf), &flen);
        if (rc != 0) {
            break;
        }
        /* seq 递增**先于** emit：emit 失败时 seq 不回退（wu_codec.h 明确要求） */
        f->seq = (uint16_t)(f->seq + 1);
        f->in_message = (i + 1 < total_frames);

        if (!emit(buf, flen, ctx)) {
            /* 端点忙：整条消息放弃（协议 §5.2 的丢弃语义 / 可靠通道靠重传） */
            f->in_message = false;
            return sent;
        }
        sent++;
        off += chunk;
    }

    f->in_message = false;
    return sent;
}

/* ============================ 消息重组器 ============================ */

int wu_assembler_init(wu_assembler_t *a, uint8_t *buf, size_t cap)
{
    if (a == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    memset(a, 0, sizeof(*a));
    a->buf = buf;
    a->cap = cap;
    return 0;
}

bool wu_assembler_push(wu_assembler_t *a, const wu_frame_t *frame, wu_msg_cb_t cb, void *ctx)
{
    if (a == NULL || frame == NULL) {
        return false;
    }

    /* ABORT：丢弃已拼装的部分（协议 §3 / §4） */
    if (frame->flags & WU_FLAG_ABORT) {
        a->stats.msg_aborted++;
        a->stats.resets++;
        a->len = 0;
        a->in_message = false;
        a->seq_valid = false;
        return false;
    }

    if (a->in_message) {
        if (!a->seq_valid || frame->seq != a->next_seq) {
            /* 上一帧消息没拼完就来了新消息（乱序/丢帧）⇒ 丢弃上一段并计数 */
            a->stats.msg_aborted++;
            a->stats.resets++;
            a->len = 0;
            a->in_message = false;
        }
    } else {
        a->len = 0; /* 单帧消息/新消息都从 0 开始 */
    }

    if (a->len + (size_t)frame->length > a->cap) {
        /* 缓冲放不下整条消息 ⇒ 丢弃（不做部分投递） */
        a->stats.msg_aborted++;
        a->stats.resets++;
        a->len = 0;
        a->in_message = false;
        a->seq_valid = false;
        return false;
    }

    if (frame->length != 0 && frame->payload != NULL) {
        memcpy(a->buf + a->len, frame->payload, frame->length);
    }
    a->len += (size_t)frame->length;
    a->next_seq = (uint16_t)(frame->seq + 1);
    a->seq_valid = true;

    if (frame->flags & WU_FLAG_MORE) {
        a->in_message = true;
        return true;
    }

    /* 收尾帧：整条消息拼完 */
    if (cb != NULL) {
        cb(a->buf, a->len, ctx);
    }
    a->stats.msg_ok++;
    a->len = 0;
    a->in_message = false;
    return true;
}
