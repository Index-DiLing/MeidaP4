// SPDX-License-Identifier: Apache-2.0
//
// ⚠️⚠️ 这不是产品代码，也**不是**被验证对象 ⚠️⚠️
//
// 用途（仅此一个）：在 `components/webusb_proto/wu_codec.c`（ws-webusb 产出）尚未落地时，
// 用一份**临时的、按 docs/protocol.md 直写**的参考编解码器把 `test/host/test_codec.c`
// （测试骨架本身）跑通，确保"测试代码 / JSON 解析 / 断言逻辑"没有 bug。
//
//   * 用它跑出的 PASS **不能**作为产品实现合规的证据 —— 它证明的是测试骨架自洽。
//   * tools/verify-proto.ps1 默认**不**使用本文件；只有 `-HarnessSelftest` 才用。
//   * wu_codec.c 落地后，本文件立即失去用途（保留仅为可复现）。
//
// 规则来源：docs/protocol.md（帧头 12 B / 小端 / CRC-16/CCITT-FALSE / resync / ZLP）

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wu_codec.h"

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t wu_crc16(const uint8_t *data, size_t len)
{
    return crc16_update(0xFFFF, data, len);
}

uint16_t wu_frame_crc(const uint8_t *hdr_first10, const uint8_t *payload, size_t payload_len)
{
    uint16_t crc = crc16_update(0xFFFF, hdr_first10, 10);
    if (payload_len > 0 && payload) {
        crc = crc16_update(crc, payload, payload_len);
    }
    return crc;
}

int wu_encode(uint8_t channel, uint16_t flags, uint16_t seq, const uint8_t *payload,
              uint16_t length, uint8_t *out, size_t cap, size_t *out_len)
{
    if (length > WU_MAX_PAYLOAD) {
        return -1;
    }
    if (length > 0 && !payload) {
        return -1;
    }
    if (!out || cap < (size_t)WU_HDR_LEN + length) {
        return -2;
    }
    out[0] = WU_MAGIC0;
    out[1] = WU_MAGIC1;
    out[2] = WU_VERSION;
    out[3] = channel;
    wu_wr16(&out[4], flags);
    wu_wr16(&out[6], length);
    wu_wr16(&out[8], seq);
    if (length > 0) {
        memcpy(out + WU_HDR_LEN, payload, length);
    }
    wu_wr16(&out[10], wu_frame_crc(out, payload, length));
    if (out_len) {
        *out_len = (size_t)WU_HDR_LEN + length;
    }
    return 0;
}

void wu_decoder_init(wu_decoder_t *d)
{
    if (!d) {
        return;
    }
    memset(d, 0, sizeof(*d));
}

static void drop_front(wu_decoder_t *d, size_t n)
{
    if (n >= d->len) {
        d->len = 0;
        return;
    }
    memmove(d->buf, d->buf + n, d->len - n);
    d->len -= n;
}

int wu_decoder_push(wu_decoder_t *d, const uint8_t *data, size_t len, wu_frame_cb_t cb, void *ctx)
{
    if (!d || len == 0) {
        return 0;   /* 0 字节 = 合法 no-op（协议 §4.1） */
    }
    int produced = 0;
    size_t off = 0;
    bool stop = false;

    while (off < len && !stop) {
        size_t space = WU_MAX_FRAME - d->len;
        size_t take = len - off;
        if (take > space) {
            take = space;
        }
        if (take > 0) {
            memcpy(d->buf + d->len, data + off, take);
            d->len += take;
            off += take;
        }

        bool need_more = false;
        for (;;) {
            if (d->len < WU_HDR_LEN) {
                need_more = (d->len > 0);
                break;
            }
            if (d->buf[0] != WU_MAGIC0 || d->buf[1] != WU_MAGIC1) {
                d->stats.resync_bytes++;
                drop_front(d, 1);
                continue;
            }
            if (d->buf[2] != WU_VERSION || wu_rd16(&d->buf[6]) > WU_MAX_PAYLOAD) {
                /* 冻结口径（wu_codec.h:148-164）：候选头自身 12 B 归 bad_header 事件、不计字节；
                 * 其后的垃圾字节计入 resync_bytes。必须与喂入粒度无关。 */
                d->stats.bad_header++;
                size_t k = 1;
                while (k + 1 < d->len && !(d->buf[k] == WU_MAGIC0 && d->buf[k + 1] == WU_MAGIC1)) {
                    k++;
                }
                bool no_magic = (k + 1 >= d->len);
                size_t skip = no_magic ? ((d->buf[d->len - 1] == WU_MAGIC0) ? (d->len - 1) : d->len) : k;
                size_t uncounted = (skip < (size_t)WU_HDR_LEN) ? skip : (size_t)WU_HDR_LEN;
                drop_front(d, uncounted);
                d->stats.resync_bytes += (uint32_t)(skip - uncounted);
                drop_front(d, skip - uncounted);
                if (no_magic) {
                    d->stats.truncated++;
                    break;
                }
                continue;
            }
            uint16_t plen = wu_rd16(&d->buf[6]);
            size_t total = (size_t)WU_HDR_LEN + plen;
            if (d->len < total) {
                need_more = true;
                break;
            }
            uint16_t crc_field = wu_rd16(&d->buf[10]);
            uint16_t crc_calc = wu_frame_crc(d->buf, d->buf + WU_HDR_LEN, plen);
            if (crc_field != crc_calc) {
                d->stats.crc_errors++;
                drop_front(d, total);
                continue;
            }
            wu_frame_t f;
            f.channel = d->buf[3];
            f.flags = wu_rd16(&d->buf[4]);
            f.seq = wu_rd16(&d->buf[8]);
            f.length = plen;
            f.payload = d->buf + WU_HDR_LEN;
            d->stats.frames++;
            produced++;
            bool cont = cb ? cb(&f, ctx) : true;
            drop_front(d, total);
            if (!cont) {
                stop = true;
                break;
            }
        }
        if (need_more) {
            d->stats.truncated++;
        }
        if (!stop && d->len == WU_MAX_FRAME) {
            /* 缓冲满且无法成帧（理论上不可达：最大帧正好等于缓冲）-> 保底丢 1 字节 */
            d->stats.resync_bytes++;
            drop_front(d, 1);
        }
    }
    return produced;
}
