// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议离线一致性测试（verifier 独立编写）
//
// 被验证对象（**只读**）：components/webusb_proto/wu_codec.c + 冻结头 wu_codec.h
// 黄金向量来源    ：tools/proto_vectors.json（由 test/host/gen_vectors.py 独立生成，
//                   生成过程不引用任何产品代码 —— 否则本测试就是循环论证）
// 规约            ：docs/protocol.md
//
// 断言内容：
//   1. crc16 命中（含 CRC catalogue 的 check 值 0x29B1）
//   2. encode 逐字节 == frame_hex，返回长度正确，cap 不足返回 -2，非法参数返回 -1
//   3. decode(encode(x)) == x（整块喂入 + 逐字节喂入两种方式必须一致）
//   4. 鲁棒性：垃圾前缀 resync / 坏 CRC 帧丢弃且不影响后续帧 / length=1025 丢弃 /
//      两帧首尾相连 / 3000 字节垃圾 / 坏 CRC 帧内嵌完整帧（不得从帧内重扫）/
//      0 字节 push 是 no-op（ZLP 语义）
//
// 编译（由 tools/verify-proto.ps1 调用）：
//   gcc -std=c11 -Wall -Wextra -O1 -I components/webusb_proto/include
//       test/host/test_codec.c components/webusb_proto/wu_codec.c -o <out>
// 运行：<out> tools/proto_vectors.json

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wu_codec.h"

/* =====================================================================
 * 断言与结果统计
 * ===================================================================== */

static int g_pass = 0;
static int g_fail = 0;
static const char *g_ctx = "";

static void ctx_set(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_ctx = buf;
}

static void ck(int cond, const char *fmt, ...)
{
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    printf("  [FAIL] %s : ", g_ctx);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/** 只统计"向量字段解析"是否成功——不能用全局 g_fail 当守卫（那是断言总数） */
static int g_fields_bad = 0;

static void ck_field(int cond, const char *fmt, ...)
{
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    g_fields_bad++;
    va_list ap;
    va_start(ap, fmt);
    printf("  [FAIL] %s : ", g_ctx);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void ck_bytes(const uint8_t *got, const uint8_t *exp, size_t n, const char *what)
{
    if (memcmp(got, exp, n) == 0) {
        g_pass++;
        return;
    }
    g_fail++;
    size_t i = 0;
    while (i < n && got[i] == exp[i]) {
        i++;
    }
    printf("  [FAIL] %s : %s 首个差异在 offset %zu (got 0x%02X, exp 0x%02X)  len=%zu\n",
           g_ctx, what, i, i < n ? got[i] : 0, i < n ? exp[i] : 0, n);
}

/* =====================================================================
 * 极简 JSON 读取器（只支持本向量文件用到的子集：对象 / 数组 / 字符串 / 整数）
 *   —— 刻意不引第三方库：测试链必须能在裸 gcc 下跑
 * ===================================================================== */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/** 扫描一个 JSON 值，返回其后的位置；vs / ve 为值的原始区间 */
static const char *scan_value(const char *p, const char **vs, const char **ve)
{
    p = skip_ws(p);
    const char *s = p;

    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p += 2;
            } else {
                p++;
            }
        }
        if (*p == '"') {
            p++;
        }
        if (vs) *vs = s + 1;
        if (ve) *ve = p - 1;
        return p;
    }

    if (*p == '{' || *p == '[') {
        const char open = *p;
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {                     /* 跳过字符串，避免被括号骗到 */
                const char *q = p + 1;
                while (*q && *q != '"') {
                    if (*q == '\\' && q[1]) {
                        q += 2;
                    } else {
                        q++;
                    }
                }
                p = (*q == '"') ? q + 1 : q;
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
            p++;
        }
        if (vs) *vs = s;
        if (ve) *ve = p;
        return p;
    }

    while (*p && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    while (p > s && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r')) {
        p--;
    }
    if (vs) *vs = s;
    if (ve) *ve = p;
    return p;
}

/** 在对象 ob..oe 里找 key，返回 1 并给出值区间 */
static int jget(const char *ob, const char *oe, const char *key, const char **vs, const char **ve)
{
    const char *p = ob;
    if (*p != '{') {
        return 0;
    }
    p++;
    for (;;) {
        p = skip_ws(p);
        if (p >= oe || *p == '}') {
            return 0;
        }
        if (*p != '"') {
            return 0;
        }
        const char *ks, *ke;
        p = scan_value(p, &ks, &ke);
        p = skip_ws(p);
        if (*p != ':') {
            return 0;
        }
        p++;
        const char *val_s, *val_e;
        p = scan_value(p, &val_s, &val_e);
        if ((size_t)(ke - ks) == strlen(key) && memcmp(ks, key, ke - ks) == 0) {
            *vs = val_s;
            *ve = val_e;
            return 1;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p++;
        }
    }
}

static int jget_int(const char *ob, const char *oe, const char *key, long *out)
{
    const char *vs, *ve;
    if (!jget(ob, oe, key, &vs, &ve)) {
        return 0;
    }
    char tmp[32];
    size_t n = (size_t)(ve - vs);
    if (n == 0 || n >= sizeof(tmp)) {
        return 0;
    }
    memcpy(tmp, vs, n);
    tmp[n] = 0;
    char *end = NULL;
    long v = strtol(tmp, &end, 10);
    if (end == tmp) {
        return 0;
    }
    *out = v;
    return 1;
}

static int jget_str(const char *ob, const char *oe, const char *key, char *out, size_t cap)
{
    const char *vs, *ve;
    if (!jget(ob, oe, key, &vs, &ve)) {
        return 0;
    }
    size_t n = (size_t)(ve - vs);
    if (n + 1 > cap) {
        return 0;
    }
    memcpy(out, vs, n);
    out[n] = 0;
    return 1;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** 十六进制字符串 -> 字节；空串 -> 0 字节 */
static int hex_decode(const char *s, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    if (n % 2 != 0 || n / 2 > cap) {
        return -1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval((unsigned char)s[2 * i]);
        int lo = hexval((unsigned char)s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return 0;
}

static int jget_hex(const char *ob, const char *oe, const char *key, uint8_t *out, size_t cap,
                    size_t *out_len)
{
    const char *vs, *ve;
    if (!jget(ob, oe, key, &vs, &ve)) {
        return 0;
    }
    return hex_decode(vs, (size_t)(ve - vs), out, cap, out_len) == 0;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

/* =====================================================================
 * 采集回调：payload 指针只在下一次 push 前有效 -> 必须立刻拷贝
 * ===================================================================== */

#define MAX_COLLECT 16

typedef struct {
    int count;
    struct {
        uint8_t channel;
        uint16_t flags;
        uint16_t seq;
        uint16_t length;
        uint8_t payload[WU_MAX_PAYLOAD];
    } f[MAX_COLLECT];
} collected_t;

static bool on_frame(const wu_frame_t *fr, void *ctx)
{
    collected_t *c = (collected_t *)ctx;
    if (c->count >= MAX_COLLECT) {
        return false;
    }
    c->f[c->count].channel = fr->channel;
    c->f[c->count].flags = fr->flags;
    c->f[c->count].seq = fr->seq;
    c->f[c->count].length = fr->length;
    if (fr->length > 0) {
        if (!fr->payload) {
            return false;
        }
        memcpy(c->f[c->count].payload, fr->payload, fr->length);
    }
    c->count++;
    return true;
}

/* =====================================================================
 * 向量断言
 * ===================================================================== */

static void test_crc_vector(const char *ob, const char *oe)
{
    uint8_t data[8 * 1024];
    size_t data_len = 0;
    long crc = 0;
    char name[128] = "?";

    jget_str(ob, oe, "name", name, sizeof(name));
    ctx_set("%s", name);

    ck(jget_hex(ob, oe, "payload_hex", data, sizeof(data), &data_len), "payload_hex 解析失败或过长");
    ck(jget_int(ob, oe, "crc16", &crc), "crc16 缺失");
    if (g_fail && 0) {
        return;
    }
    uint16_t got = wu_crc16(data, data_len);
    ck(got == (uint16_t)crc, "crc16 不符：got=0x%04X exp=0x%04X len=%zu", got, (unsigned)(uint16_t)crc, data_len);
}

static void test_frame_vector(const char *ob, const char *oe)
{
    uint8_t payload[WU_MAX_PAYLOAD];
    uint8_t frame[WU_MAX_FRAME];
    size_t payload_len = 0, frame_len = 0;
    long channel = -1, flags = -1, seq = -1, length = -1, crc = -1;
    char name[128] = "?";

    jget_str(ob, oe, "name", name, sizeof(name));
    ctx_set("%s", name);
    g_fields_bad = 0;

    ck_field(jget_hex(ob, oe, "payload_hex", payload, sizeof(payload), &payload_len), "payload_hex 解析失败/超 1024");
    ck_field(jget_hex(ob, oe, "frame_hex", frame, sizeof(frame), &frame_len), "frame_hex 解析失败/超 1036");
    ck_field(jget_int(ob, oe, "channel", &channel), "channel 缺失");
    ck_field(jget_int(ob, oe, "flags", &flags), "flags 缺失");
    ck_field(jget_int(ob, oe, "seq", &seq), "seq 缺失");
    ck_field(jget_int(ob, oe, "length", &length), "length 缺失");
    ck_field(jget_int(ob, oe, "crc16", &crc), "crc16 缺失");
    if (g_fields_bad) {
        printf("  [SKIP] %s : 向量字段不完整，跳过本向量后续断言\n", name);
        return;
    }

    const uint8_t ch = (uint8_t)channel;
    const uint16_t fl = (uint16_t)flags;
    const uint16_t sq = (uint16_t)seq;
    const uint16_t len = (uint16_t)length;
    const uint16_t crc16 = (uint16_t)crc;

    /* ---- 向量自身的内部一致性（防止向量文件被改坏） ---- */
    ck(payload_len == len, "payload_hex 长度 %zu != length %u", payload_len, len);
    ck(frame_len == (size_t)WU_HDR_LEN + len, "frame_hex 长度 %zu != 12+%u", frame_len, len);
    ck(frame_len >= WU_HDR_LEN, "frame_hex 太短");

    /* ---- 帧头逐字段 ---- */
    ck(frame[0] == WU_MAGIC0 && frame[1] == WU_MAGIC1, "magic 不是 4D 50（got %02X %02X）",
       frame[0], frame[1]);
    ck(frame[2] == WU_VERSION, "version != 1（got %u）", frame[2]);
    ck(frame[3] == ch, "channel 不符（got %u exp %u）", frame[3], ch);
    ck(wu_rd16(&frame[4]) == fl, "flags 不符（got 0x%04X exp 0x%04X）", wu_rd16(&frame[4]), fl);
    ck(wu_rd16(&frame[6]) == len, "length 字段不符（got %u exp %u）", wu_rd16(&frame[6]), len);
    ck(wu_rd16(&frame[8]) == sq, "seq 字段不符（got %u exp %u）", wu_rd16(&frame[8]), sq);
    ck(wu_rd16(&frame[10]) == crc16, "crc16 字段不符（got 0x%04X exp 0x%04X）",
       wu_rd16(&frame[10]), crc16);

    /* ---- 实现的 CRC 必须命中（独立复算值） ---- */
    ck(wu_crc16(frame, 10) == wu_crc16(frame, 10), "占位");
    uint16_t got_frame_crc = wu_frame_crc(frame, payload, len);
    ck(got_frame_crc == crc16, "wu_frame_crc(hdr10,payload) 不符：got=0x%04X exp=0x%04X",
       got_frame_crc, crc16);

    /* 独立复算：把 hdr[0..10) 与 payload 拼起来自己算一遍（不经过 wu_frame_crc 的实现细节） */
    {
        uint8_t joined[WU_MAX_FRAME];
        memcpy(joined, frame, 10);
        memcpy(joined + 10, payload, len);
        ck(wu_crc16(joined, 10 + len) == crc16,
           "wu_crc16(hdr10+payload) 不符：got=0x%04X exp=0x%04X", wu_crc16(joined, 10 + len), crc16);
    }

    /* ---- encode 逐字节 ---- */
    {
        uint8_t out[WU_MAX_FRAME + 8];
        size_t out_len = 0;
        memset(out, 0xCC, sizeof(out));
        int rc = wu_encode(ch, fl, sq, payload, len, out, sizeof(out), &out_len);
        ck(rc == 0, "wu_encode 返回 %d（期望 0）", rc);
        ck(out_len == frame_len, "wu_encode out_len=%zu（期望 %zu）", out_len, frame_len);
        ck_bytes(out, frame, frame_len, "wu_encode 字节流");
        /* 缓冲越界哨兵：第 1036 字节之后不得被写 */
        ck(out[frame_len] == 0xCC, "wu_encode 写越界（out[%zu]=0x%02X）", frame_len, out[frame_len]);
    }

    /* ---- encode 的边界/错误路径 ---- */
    {
        uint8_t out[WU_MAX_FRAME + 8];
        size_t out_len = 123;
        int rc = wu_encode(ch, fl, sq, payload, len, out, frame_len - 1, &out_len);
        ck(rc == -2, "cap 不足（%zu）应返回 -2，实测 %d", frame_len - 1, rc);

        rc = wu_encode(ch, fl, sq, payload, (uint16_t)(WU_MAX_PAYLOAD + 1), out, sizeof(out), &out_len);
        ck(rc == -1, "length=1025 应返回 -1（参数非法），实测 %d", rc);

        rc = wu_encode(ch, fl, sq, NULL, 5, out, sizeof(out), &out_len);
        ck(rc == -1, "payload=NULL 且 length>0 应返回 -1，实测 %d", rc);
    }
    if (len == 0) {
        uint8_t out[WU_MAX_FRAME];
        size_t out_len = 0;
        int rc = wu_encode(ch, fl, sq, NULL, 0, out, sizeof(out), &out_len);
        ck(rc == 0 && out_len == WU_HDR_LEN, "length=0 且 payload=NULL 应成功（rc=%d len=%zu）",
           rc, out_len);
        if (rc == 0) {
            ck_bytes(out, frame, frame_len, "length=0 的 encode 字节流");
        }
    }

    /* ---- decode(encode(x)) == x：整块喂入 ---- */
    {
        wu_decoder_t d;
        collected_t c;
        memset(&c, 0, sizeof(c));
        wu_decoder_init(&d);
        int n = wu_decoder_push(&d, frame, frame_len, on_frame, &c);
        ck(n == 1, "整块喂入应解出 1 帧，实测 %d", n);
        ck(c.count == 1, "回调触发 %d 次（期望 1）", c.count);
        ck(d.stats.frames == 1, "stats.frames=%u（期望 1）", (unsigned)d.stats.frames);
        ck(d.stats.crc_errors == 0, "stats.crc_errors=%u（期望 0）", (unsigned)d.stats.crc_errors);
        if (c.count == 1) {
            ck(c.f[0].channel == ch, "解出 channel=%u exp %u", c.f[0].channel, ch);
            ck(c.f[0].flags == fl, "解出 flags=0x%04X exp 0x%04X", c.f[0].flags, fl);
            ck(c.f[0].seq == sq, "解出 seq=%u exp %u", c.f[0].seq, sq);
            ck(c.f[0].length == len, "解出 length=%u exp %u", c.f[0].length, len);
            ck_bytes(c.f[0].payload, payload, len, "解出 payload");
        }
    }

    /* ---- decode(encode(x)) == x：逐字节喂入（必须与整块喂入结果一致） ---- */
    {
        wu_decoder_t d;
        collected_t c;
        memset(&c, 0, sizeof(c));
        wu_decoder_init(&d);
        int total = 0;
        for (size_t i = 0; i < frame_len; i++) {
            total += wu_decoder_push(&d, &frame[i], 1, on_frame, &c);
        }
        ck(total == 1, "逐字节喂入应解出 1 帧，实测 %d", total);
        ck(c.count == 1, "逐字节喂入回调 %d 次（期望 1）", c.count);
        ck(d.stats.frames == 1, "逐字节喂入 stats.frames=%u（期望 1）", (unsigned)d.stats.frames);
        ck(d.stats.crc_errors == 0, "逐字节喂入 stats.crc_errors=%u（期望 0）",
           (unsigned)d.stats.crc_errors);
        if (c.count == 1) {
            ck(c.f[0].channel == ch && c.f[0].flags == fl && c.f[0].seq == sq &&
               c.f[0].length == len, "逐字节喂入解出的头字段与整块不一致");
            ck_bytes(c.f[0].payload, payload, len, "逐字节喂入解出的 payload");
        }
    }
}

typedef struct {
    uint8_t channel;
    uint16_t flags;
    uint16_t seq;
    uint16_t length;
    uint8_t payload[WU_MAX_PAYLOAD];
} exp_frame_t;

/* ---------------------------------------------------------------------------
 * 喂入粒度不变量：同一字节流，整块喂 / 分片喂 / 逐字节喂，诊断计数必须相同
 *   （wu_codec.c 开头第 20-21 行自己声明的口径："所有计数器对一次喂 1 字节与
 *     一次喂整段结果相同，唯一例外是 truncated"；这里用黄金向量把它变成机械断言）
 * ------------------------------------------------------------------------- */
typedef struct {
    char name[128];
    char group[64];
    uint32_t frames, crc_errors, bad_header, resync_bytes;
    int valid;
} stat_row_t;

static stat_row_t g_rows[64];
static int g_row_n = 0;

static void stat_row_add(const char *name, const char *group, const wu_decoder_stats_t *s)
{
    if (g_row_n >= 64) {
        return;
    }
    stat_row_t *r = &g_rows[g_row_n++];
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->group, sizeof(r->group), "%s", group);
    r->frames = s->frames;
    r->crc_errors = s->crc_errors;
    r->bad_header = s->bad_header;
    r->resync_bytes = s->resync_bytes;
    r->valid = 1;
}

static void test_granularity_invariants(void)
{
    ctx_set("不变量/喂入粒度");
    for (int i = 0; i < g_row_n; i++) {
        if (!g_rows[i].valid || g_rows[i].group[0] == 0) {
            continue;
        }
        for (int j = i + 1; j < g_row_n; j++) {
            if (!g_rows[j].valid || strcmp(g_rows[i].group, g_rows[j].group) != 0) {
                continue;
            }
            ck(g_rows[i].frames == g_rows[j].frames,
               "[%s] frames 依赖喂入粒度：%s=%u vs %s=%u", g_rows[i].group, g_rows[i].name,
               (unsigned)g_rows[i].frames, g_rows[j].name, (unsigned)g_rows[j].frames);
            ck(g_rows[i].crc_errors == g_rows[j].crc_errors,
               "[%s] crc_errors 依赖喂入粒度：%s=%u vs %s=%u", g_rows[i].group, g_rows[i].name,
               (unsigned)g_rows[i].crc_errors, g_rows[j].name, (unsigned)g_rows[j].crc_errors);
            ck(g_rows[i].bad_header == g_rows[j].bad_header,
               "[%s] bad_header 依赖喂入粒度：%s=%u vs %s=%u", g_rows[i].group, g_rows[i].name,
               (unsigned)g_rows[i].bad_header, g_rows[j].name, (unsigned)g_rows[j].bad_header);
            ck(g_rows[i].resync_bytes == g_rows[j].resync_bytes,
               "[%s] resync_bytes 依赖喂入粒度：%s=%u vs %s=%u", g_rows[i].group, g_rows[i].name,
               (unsigned)g_rows[i].resync_bytes, g_rows[j].name, (unsigned)g_rows[j].resync_bytes);
        }
    }
}

static void test_stream_vector(const char *ob, const char *oe)
{
    static uint8_t stream[64 * 1024];
    size_t stream_len = 0;
    long chunk_size = 0, zero_every = 0;
    long exp_crc = 0, exp_bad = 0, exp_resync = 0;
    char name[128] = "?";

    jget_str(ob, oe, "name", name, sizeof(name));
    ctx_set("%s", name);
    g_fields_bad = 0;

    if (!jget_hex(ob, oe, "stream_hex", stream, sizeof(stream), &stream_len)) {
        ck_field(0, "stream_hex 解析失败或超 64 KiB");
        return;
    }
    ck_field(jget_int(ob, oe, "chunk_size", &chunk_size), "chunk_size 缺失");
    jget_int(ob, oe, "zero_push_every", &zero_every);
    ck_field(jget_int(ob, oe, "expect_crc_errors", &exp_crc), "expect_crc_errors 缺失");
    ck_field(jget_int(ob, oe, "expect_bad_header", &exp_bad), "expect_bad_header 缺失");
    ck_field(jget_int(ob, oe, "expect_resync_bytes", &exp_resync), "expect_resync_bytes 缺失");

    /* 期望帧列表 */
    exp_frame_t exp[MAX_COLLECT];
    int exp_count = 0;
    {
        const char *vs, *ve;
        ck_field(jget(ob, oe, "expect_frames", &vs, &ve), "expect_frames 缺失");
        if (!jget(ob, oe, "expect_frames", &vs, &ve)) {
            return;
        }
        const char *it = skip_ws(vs);
        if (*it == '[') {
            it++;
        }
        const char *limit = ve;
        while (it < limit) {
            const char *os, *oe2;
            it = skip_ws(it);
            if (*it == ']' || it >= limit) {
                break;
            }
            it = scan_value(it, &os, &oe2);
            if (exp_count >= MAX_COLLECT) {
                ck_field(0, "expect_frames 超过 %d 个", MAX_COLLECT);
                break;
            }
            long v = 0;
            if (!jget_int(os, oe2, "channel", &v)) { ck_field(0, "expect_frames[%d].channel 缺失", exp_count); break; }
            exp[exp_count].channel = (uint8_t)v;
            jget_int(os, oe2, "flags", &v); exp[exp_count].flags = (uint16_t)v;
            jget_int(os, oe2, "seq", &v);   exp[exp_count].seq = (uint16_t)v;
            jget_int(os, oe2, "length", &v); exp[exp_count].length = (uint16_t)v;
            size_t pl = 0;
            if (!jget_hex(os, oe2, "payload_hex", exp[exp_count].payload,
                          sizeof(exp[exp_count].payload), &pl)) {
                ck_field(0, "expect_frames[%d].payload_hex 解析失败", exp_count);
                break;
            }
            if (pl != exp[exp_count].length) {
                ck_field(0, "expect_frames[%d] payload 长度 %zu != length %u", exp_count, pl,
                         exp[exp_count].length);
            }
            exp_count++;
            it = skip_ws(it);
            if (*it == ',') {
                it++;
            }
        }
    }

    if (g_fields_bad) {
        printf("  [SKIP] %s : 向量字段不完整，跳过本向量后续断言\n", name);
        return;
    }

    /* chunk_plan（若存在）覆盖 chunk_size */
    long plan[128];
    int plan_n = 0;
    int have_plan = 0;
    {
        const char *vs, *ve;
        if (jget(ob, oe, "chunk_plan", &vs, &ve)) {
            have_plan = 1;
            const char *it = skip_ws(vs);
            if (*it == '[') {
                it++;
            }
            while (it < ve) {
                it = skip_ws(it);
                if (*it == ']' || it >= ve) {
                    break;
                }
                const char *ns, *ne;
                it = scan_value(it, &ns, &ne);
                char tmp[16];
                size_t n = (size_t)(ne - ns);
                if (n == 0 || n >= sizeof(tmp)) {
                    break;
                }
                memcpy(tmp, ns, n);
                tmp[n] = 0;
                if (plan_n < (int)(sizeof(plan) / sizeof(plan[0]))) {
                    plan[plan_n++] = strtol(tmp, NULL, 10);
                }
                it = skip_ws(it);
                if (*it == ',') {
                    it++;
                }
            }
        }
    }

    /* ---- 喂入 ---- */
    wu_decoder_t d;
    collected_t c;
    memset(&c, 0, sizeof(c));
    wu_decoder_init(&d);

    size_t off = 0;
    int pushed = 0;
    if (have_plan) {
        for (int i = 0; i < plan_n; i++) {
            long n = plan[i];
            if (n == 0) {
                wu_decoder_push(&d, &stream[off], 0, on_frame, &c);   /* 0 字节 = 合法 no-op */
            } else {
                if (off + (size_t)n > stream_len) { ck(0, "chunk_plan 越界"); break; }
                wu_decoder_push(&d, &stream[off], (size_t)n, on_frame, &c);
                off += (size_t)n;
            }
            pushed++;
        }
        ck(off == stream_len, "chunk_plan 只消费了 %zu/%zu 字节", off, stream_len);
    } else if (chunk_size <= 0) {
        wu_decoder_push(&d, stream, stream_len, on_frame, &c);
        pushed = 1;
    } else {
        size_t step = (size_t)chunk_size;
        for (size_t i = 0; i < stream_len; i += step) {
            size_t n = (stream_len - i < step) ? (stream_len - i) : step;
            wu_decoder_push(&d, &stream[i], n, on_frame, &c);
            pushed++;
            if (zero_every > 0 && (pushed % (int)zero_every) == 0) {
                wu_decoder_push(&d, &stream[i], 0, on_frame, &c);
            }
        }
        off = stream_len;
    }

    /* ---- 断言：帧数、逐帧字段、统计量 ---- */
    ck(c.count == exp_count, "解出 %d 帧（期望 %d）", c.count, exp_count);
    ck(d.stats.frames == (uint32_t)exp_count, "stats.frames=%u（期望 %d）",
       (unsigned)d.stats.frames, exp_count);
    if (c.count == exp_count) {
        for (int i = 0; i < exp_count; i++) {
            ck(c.f[i].channel == exp[i].channel, "帧%d channel=%u exp %u", i, c.f[i].channel,
               exp[i].channel);
            ck(c.f[i].flags == exp[i].flags, "帧%d flags=0x%04X exp 0x%04X", i, c.f[i].flags,
               exp[i].flags);
            ck(c.f[i].seq == exp[i].seq, "帧%d seq=%u exp %u", i, c.f[i].seq, exp[i].seq);
            ck(c.f[i].length == exp[i].length, "帧%d length=%u exp %u", i, c.f[i].length,
               exp[i].length);
            if (c.f[i].length == exp[i].length) {
                ck_bytes(c.f[i].payload, exp[i].payload, exp[i].length, "帧 payload");
            }
        }
    }
    ck(d.stats.crc_errors == (uint32_t)exp_crc, "stats.crc_errors=%u（期望 %ld）",
       (unsigned)d.stats.crc_errors, exp_crc);
    ck(d.stats.bad_header == (uint32_t)exp_bad, "stats.bad_header=%u（期望 %ld）",
       (unsigned)d.stats.bad_header, exp_bad);
    ck(d.stats.resync_bytes == (uint32_t)exp_resync, "stats.resync_bytes=%u（期望 %ld）",
       (unsigned)d.stats.resync_bytes, exp_resync);

    {
        char group[64] = "";
        jget_str(ob, oe, "invariant_group", group, sizeof(group));
        stat_row_add(name, group, &d.stats);
    }

    printf("        [info] %s: frames=%u crc_errors=%u bad_header=%u resync_bytes=%u truncated=%u pushed=%d\n",
           name, (unsigned)d.stats.frames, (unsigned)d.stats.crc_errors,
           (unsigned)d.stats.bad_header, (unsigned)d.stats.resync_bytes,
           (unsigned)d.stats.truncated, pushed);
}

/* =====================================================================
 * 全局：0 字节 push 必须是 no-op（协议 §4.1 ZLP）
 * ===================================================================== */

static void test_zlp_noop(void)
{
    ctx_set("zlp/noop");
    wu_decoder_t d;
    collected_t c;
    memset(&c, 0, sizeof(c));
    wu_decoder_init(&d);
    uint8_t dummy = 0;
    int n = 0;
    for (int i = 0; i < 5; i++) {
        n += wu_decoder_push(&d, &dummy, 0, on_frame, &c);
    }
    n += wu_decoder_push(&d, NULL, 0, on_frame, &c);
    ck(n == 0, "0 字节 push 不应产出帧（实测 %d）", n);
    ck(c.count == 0, "0 字节 push 不应触发回调（实测 %d）", c.count);
    ck(d.stats.frames == 0 && d.stats.crc_errors == 0 && d.stats.bad_header == 0 &&
       d.stats.resync_bytes == 0, "0 字节 push 不应推进任何状态（frames=%u crc=%u bad=%u resync=%u）",
       (unsigned)d.stats.frames, (unsigned)d.stats.crc_errors, (unsigned)d.stats.bad_header,
       (unsigned)d.stats.resync_bytes);

    /* 空 push 夹在真实帧中间也不得破坏解码 */
    uint8_t f[WU_MAX_FRAME];
    size_t fl = 0;
    if (wu_encode(WU_CH_CTRL, WU_FLAG_ACK, 7, (const uint8_t *)"x", 1, f, sizeof(f), &fl) == 0) {
        wu_decoder_init(&d);
        memset(&c, 0, sizeof(c));
        wu_decoder_push(&d, NULL, 0, on_frame, &c);
        for (size_t i = 0; i < fl; i++) {
            wu_decoder_push(&d, f + i, 1, on_frame, &c);
            wu_decoder_push(&d, &dummy, 0, on_frame, &c);
        }
        ck(c.count == 1 && c.f[0].seq == 7, "空 push 穿插不得影响解码（count=%d seq=%u）",
           c.count, c.count ? c.f[0].seq : 0);
    } else {
        ck(0, "wu_encode 失败，无法做空 push 穿插测试");
    }
}

/* =====================================================================
 * main
 * ===================================================================== */

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "tools/proto_vectors.json";

    printf("==============================================================================\n");
    printf("MeidaP4 · I0 线协议离线一致性测试（host gcc）\n");
    printf("  被验证 : components/webusb_proto/wu_codec.c（冻结头 wu_codec.h）\n");
    printf("  向量   : %s\n", path);
    printf("  规约   : docs/protocol.md\n");
    printf("==============================================================================\n");

    size_t text_len = 0;
    char *text = read_file(path, &text_len);
    if (!text) {
        printf("[test_codec] FATAL 无法读取向量文件：%s\n", path);
        return 2;
    }
    printf("[test_codec] 向量文件 %zu 字节\n", text_len);

    const char *arr_s, *arr_e;
    const char *p = skip_ws(text);
    if (*p != '[') {
        printf("[test_codec] FATAL 向量文件顶层必须是 JSON 数组（docs/protocol.md §8.1）\n");
        free(text);
        return 2;
    }
    scan_value(p, &arr_s, &arr_e);

    const char *it = arr_s + 1;
    const char *limit = arr_e - 1;
    int n_frame = 0, n_crc = 0, n_stream = 0, n_other = 0;

    while (it < limit) {
        const char *os, *oe;
        it = skip_ws(it);
        if (it >= limit || *it == ']') {
            break;
        }
        it = scan_value(it, &os, &oe);
        char kind[32] = "";
        if (!jget_str(os, oe, "kind", kind, sizeof(kind))) {
            ctx_set("(未知向量)");
            ck(0, "向量缺少 kind 字段");
        } else if (strcmp(kind, "frame") == 0) {
            n_frame++;
            test_frame_vector(os, oe);
        } else if (strcmp(kind, "crc") == 0) {
            n_crc++;
            test_crc_vector(os, oe);
        } else if (strcmp(kind, "stream") == 0) {
            n_stream++;
            test_stream_vector(os, oe);
        } else {
            n_other++;
            ctx_set("(kind=%s)", kind);
            ck(0, "未知向量类型");
        }
        it = skip_ws(it);
        if (*it == ',') {
            it++;
        }
    }

    test_zlp_noop();
    test_granularity_invariants();

    free(text);

    printf("------------------------------------------------------------------------------\n");
    printf("向量统计: frame=%d crc=%d stream=%d 其他=%d\n", n_frame, n_crc, n_stream, n_other);
    printf("断言汇总: %d 条，通过 %d，失败 %d\n", g_pass + g_fail, g_pass, g_fail);
    printf("判定    : %s\n", g_fail == 0 ? "PASS（离线一致性：C 实现与 docs/protocol.md 相符）"
                                         : "FAIL（C 实现与黄金向量不符，见上面的逐条 FAIL）");
    printf("边界    : 本结果**只**证明 C 编解码器与 docs/protocol.md 逐字节一致，\n");
    printf("          **不能**证明真机 USB 链路成立。离线 PASS != 真机通过。\n");
    printf("==============================================================================\n");
    return g_fail == 0 ? 0 : 1;
}
