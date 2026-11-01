// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · IF1 · **Vendor 视频**（只有 1 个 bulk IN，独占 EP3）
//
// 分配：docs/contracts/frozen-allocation.md §1/§2（IF1 FF/00/00，EP3 bulk IN，HS 512 / FS 64，
//       **不发 WCID**）。接口号/端点号/字符串索引全部取自 `usb_fn_alloc_t`，本文件不硬编码。
// 数据源：I2 契约 `components/video_pipe`（WS-video 实现；本文件只调用，不实现）。
//   ⚠️ 2026-10 Lead 裁决：I2 是**强依赖**，不用 weak 引用 —— probe-report §3.2 的教训是
//   弱符号会把"某组件没被链接"这类真实缺陷掩盖成"暂时没帧"。见 CMakeLists 的 REQUIRES。
//   I2 的帧类型已改名为 `video_frame_t`（原来与 I0 的 `wu_frame_t` 重名），故两份头可共存。
// 帧格式：docs/protocol.md §5.2（16 字节定长视频头 + 编码数据分片；一帧 = 一条消息）。
// 丢弃策略：§5.2 —— **永不阻塞**视频发送，端点忙就丢整条消息并 `video_pipe_report_drop(1)`。

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "usb_composite.h"

#include "webusb_proto.h"   /* I0 设备侧服务（wu_encode / WU_* 常量 / link 状态 / ep_write） */
#include "video_pipe.h"     /* I2 帧源（强依赖） */

#include "usb_vendor_priv.h"
#include "usb_vendor.h"

static const char *TAG = "uv_video";

/** §5.2：payload = 16 B 视频头 + 编码数据 ⇒ 每片最多带这么多编码字节 */
#define UV_VIDEO_CHUNK_DATA  (WU_MAX_PAYLOAD - WU_VIDEO_HDR_LEN)   /* 1008 */

/**
 * 背压高水位（字节）：`usb_composite_ep_write_pending()` 现在返回驱动 TX FIFO 的占用。
 * FIFO 总 8192 B；积压超过一半才说明主机真的没在收（或远远跟不上），此时跳帧；
 * 否则"FIFO 里有一点残余"是正常在途数据，不该丢帧。
 */
#define UV_VIDEO_BACKPRESSURE_BYTES  4096

static uint8_t      s_ep_in;
static bool         s_bound;
static uint16_t     s_tx_seq;
static TaskHandle_t s_task;

/* =========================================================================
 * 描述符片段（I1：只生成本功能的 IF + EP，不含 Config 描述符）
 * ========================================================================= */

static esp_err_t video_build_desc(bool high_speed, const usb_fn_alloc_t *alloc,
                                  uint8_t *buf, size_t cap, size_t *out_len)
{
    uint16_t mps = high_speed ? UV_BULK_MPS_HS : UV_BULK_MPS_FS;
    size_t need = USB_ITF_DESC_LEN + USB_EP_DESC_LEN;
    size_t n = 0;

    if (alloc == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (alloc->itf_count < 1 || alloc->ep_in_count < 1) {
        return ESP_ERR_INVALID_STATE;   /* 复合层给的号码不完整 */
    }
    if (cap < need) {
        return ESP_ERR_INVALID_SIZE;
    }
    n += uv_itf_desc(buf + n, alloc->itf[0], 1, alloc->itf_str_idx);
    n += uv_ep_desc(buf + n, alloc->ep_in[0], mps, 0);
    *out_len = n;
    return ESP_OK;
}

static esp_err_t video_init(const usb_fn_alloc_t *alloc)
{
    if (alloc == NULL || alloc->itf_count != 1 || alloc->ep_in_count != 1) {
        ESP_LOGE(TAG, "I1 分配异常：itf_count=%u ep_in_count=%u（视频功能固定 1 接口 / 1 IN）",
                 alloc ? (unsigned)alloc->itf_count : 0u, alloc ? (unsigned)alloc->ep_in_count : 0u);
        return ESP_ERR_INVALID_STATE;
    }
    s_ep_in = alloc->ep_in[0];
    s_tx_seq = 0;
    s_bound = true;
    ESP_LOGI(TAG, "IF1 视频已绑定：itf=%u ep_in=0x%02X（只有 IN；无 OUT）str_idx=%u",
             (unsigned)alloc->itf[0], (unsigned)s_ep_in, (unsigned)alloc->itf_str_idx);
    return ESP_OK;
}

/* =========================================================================
 * 发送（§5.2）
 * ========================================================================= */

static void video_fps(uint16_t *num, uint16_t *den)
{
    video_pipe_stats_t st;
    uint32_t x100;

    memset(&st, 0, sizeof(st));
    video_pipe_get_stats(&st);
    x100 = st.fps_actual_x100;

    if (x100 == 0) {
        *num = 30;      /* I2 默认档 720p30（video_pipe_config_default） */
        *den = 1;
    } else {
        *num = (uint16_t)((x100 > 0xFFFFu) ? 0xFFFFu : x100);
        *den = 100;     /* fps_actual_x100 是以 100 为分母的实测帧率 */
    }
}

/**
 * @brief 把一整帧编码数据分片发到 EP3。
 * @return true = 整帧的所有分片都完整写进 TX FIFO；false = 预算内没发下（丢弃剩余分片并报背压）
 *
 * ⚠️ 2026-10-22 T2 真机修正：旧实现是"一次 webusb_proto_ep_write，返回 < flen 就整帧作废"。
 *    8192 B 的 vendor TX FIFO 在 720p 帧（≈48 KB）面前必然写一半就满，于是**每一帧都被判废**
 *    （实测 frames_sent=0、dropped 涨、主机一帧都收不到）。现在改成"在时间预算内把每条消息
 *    完整塞进 FIFO"——视频任务阻塞几毫秒是可以接受的，USB 栈本身不被阻塞（§4.1-3 的意图）。
 */
#define UV_VIDEO_MSG_BUDGET_MS  100   /* 单条消息（≤1036 B）最长等待；超时=链路真的堵了 */

static bool video_write_msg(const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    while (sent < len) {
        size_t w = webusb_proto_ep_write(s_ep_in, buf + sent, len - sent);
        if (w != 0) {
            sent += w;
            continue;
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((uint32_t)(now - t0) >= UV_VIDEO_MSG_BUDGET_MS) {
            return false;
        }
        vTaskDelay(1);   /* FIFO 满：让 TinyUSB 把在途数据发出去 */
    }
    return true;
}

/**
 * @brief 把一整帧编码数据发出去（**协议 §5.2：一条消息 = 一整个编码帧**）。
 *
 * ⚠️ 2026-10-31 真机修正（画面花屏/错位的根因）：
 *   旧实现把 **16 字节视频头塞进了每一个协议帧**（每个 1036 B 帧 = 头 + 1008 B 数据），
 *   并且用 MORE 标志表示"同一帧还有下一片"。
 *   但 docs/protocol.md §5.2 的语义是：
 *     * 一条消息 = 整个编码帧；
 *     * 16 B 视频头只在消息**开头出现一次**；
 *     * 消息超过 WU_MAX_PAYLOAD(1024) 时，按普通协议分片（MORE=1 表示"同一条消息还有分片"）。
 *   网页侧（web/app_src/js/40_video.js）严格按协议实现：它用 Assembler 把带 MORE 的分片
 *   拼成整条消息、然后从消息开头解析 16 B 头。于是旧固件发出去的每一个"自带 16 B 头"的
 *   1036 B 帧，除了第一片以外，**每 16 B 都被当成 JPEG 数据拼进图像** ⇒ 浏览器解码出
 *   "横向错位 + 颜色错乱"的花屏（主机 Qt/Python 工具因为按旧语义逐帧解析，反而看起来正常，
 *   掩盖了这个 bug）。
 *
 * 现在的发送形态（与协议一致，也与网页/其它客户端一致）：
 *   frag0: [16 B 视频头][最多 1008 B JPEG]        MORE=1（还有）
 *   fragN: [最多 1024 B JPEG 续片]                MORE=1/0
 *   frame_seq 只在完整帧上 +1；chunk_index 恒 0。
 */
#define UV_VIDEO_FRAME_BUDGET_MS  1500  /* 整帧发送预算：超了就丢帧，避免长期占着 slot */

static bool video_send_message(const video_frame_t *f)
{
    size_t off = 0;
    size_t vhdr_len = WU_VIDEO_HDR_LEN;
    size_t first_cap = WU_MAX_PAYLOAD - vhdr_len;   /* 1008 */
    uint16_t fps_num, fps_den;
    uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool first = true;

    if (f->data == NULL && f->len != 0) {
        return false;
    }
    video_fps(&fps_num, &fps_den);

    do {
        uint8_t  payload[WU_MAX_PAYLOAD];
        uint8_t  frame[WU_MAX_FRAME];
        size_t   flen = 0;
        size_t   cap = first ? first_cap : (size_t)WU_MAX_PAYLOAD;
        size_t   n = f->len - off;
        size_t   plen;
        uint16_t flags;

        if (n > cap) {
            n = cap;
        }
        plen = n + (first ? vhdr_len : 0);

        if (first) {
            /* §5.2 的 16 字节视频头：**整条消息只出现一次**（chunk_index 恒 0） */
            payload[0] = (uint8_t)f->fmt;
            payload[1] = f->keyframe ? 0x01 : 0x00;
            wu_wr16(&payload[2], f->width);
            wu_wr16(&payload[4], f->height);
            wu_wr16(&payload[6], fps_num);
            wu_wr16(&payload[8], fps_den);
            wu_wr32(&payload[10], f->frame_seq);
            wu_wr16(&payload[14], 0);   /* chunk_index = 0（一条消息 = 一帧） */
        }
        if (n != 0) {
            memcpy(&payload[(first ? vhdr_len : 0)], f->data + off, n);
        }
        off += n;

        /* MORE=1 表示"同一条消息还有分片"（视频不可靠：不置 ACK 位） */
        flags = (uint16_t)((off < f->len) ? WU_FLAG_MORE : 0);
        if (wu_encode(WU_CH_VIDEO, flags, s_tx_seq, payload, (uint16_t)plen,
                      frame, sizeof(frame), &flen) != 0) {
            return false;
        }
        s_tx_seq = (uint16_t)(s_tx_seq + 1);
        first = false;

        if (!video_write_msg(frame, flen)) {
            return false;
        }

        /* 整帧预算：超时就丢帧（接收方会因缺 MORE=0 收尾而丢弃整条消息，§5.2） */
        if (off < f->len) {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((uint32_t)(now - t0) >= UV_VIDEO_FRAME_BUDGET_MS) {
                return false;
            }
        }
    } while (off < f->len);

    return true;
}

static void video_task(void *arg)
{
    uint32_t last_gen = 0xFFFFFFFFu;
    (void)arg;

    for (;;) {
        video_frame_t f;

        if (!s_bound || !usb_composite_is_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* 链路"代"变化 = 序号被复位（协议 §6.3：收到 HELLO_ACK 时把自己所有 TX seq 归 0） */
        {
            uint32_t gen = webusb_proto_link_generation();
            if (gen != last_gen) {
                last_gen = gen;
                s_tx_seq = 0;
            }
        }
        if (!webusb_proto_is_link_up()) {
            vTaskDelay(pdMS_TO_TICKS(50));   /* 主机还没握手完，没人收视频 */
            continue;
        }

        /* §4.1-3 的"端点残留旧字节"由 §5.2 的帧头 + CRC + MORE=0 收尾兜底；
           ⚠️ 2026-10-22 T2 真机修正：这里**不要再**按"FIFO 占用 > 阈值就跳帧"。
           video_write_msg() 已经会在预算内把每条消息完整塞进 FIFO，FIFO 占用只是
           "USB 正在发"的正常在途数据；旧的门控在真机上导致 stale_skip=843（几乎每帧都被跳过）。
           真正的背压由 video_write_msg() 超时 → 返回 false → 丢整帧 + report_drop 处理。 */

        memset(&f, 0, sizeof(f));
        {
            esp_err_t e = video_pipe_get_frame(&f, 20);
            if (e == ESP_ERR_TIMEOUT) {
                continue;   /* 正常：还没有新帧 */
            }
            if (e != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(100));   /* 管线内部错误：不忙等 */
                continue;
            }
        }

        if (video_send_message(&f)) {
            g_uv_stats.video_frames_sent++;
            g_uv_stats.video_bytes_sent += (uint32_t)f.len;
        } else {
            g_uv_stats.video_frames_dropped++;
            video_pipe_report_drop(1);   /* 背压：让管线跳帧/降质量，别耗尽 PSRAM */
        }
        video_pipe_release_frame(&f);    /* I2：借出的帧必须在下次取帧前归还 */
    }
}

esp_err_t uv_video_task_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(video_task, "uv_video", 6144, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "视频发送任务已启动（等 mount + link_up 后按 §5.2 分片发 EP3）");
    return ESP_OK;
}

/* =========================================================================
 * I1 功能对象
 * ========================================================================= */

const usb_function_t usb_vendor_fn_video = {
    .name = "vendor-video",
    .itf_count = 1,
    .itf_string = "MeidaP4 WebUSB",   /* 与 IF2 共用同一个字符串索引（frozen-allocation §4） */
    .extra_string = NULL,
    .use_iad = false,                 /* 单接口功能不加 IAD（§6） */
    .ep_in_req = {
        [0] = { .xfer = USB_EP_XFER_BULK, .mps_hs = UV_BULK_MPS_HS, .mps_fs = UV_BULK_MPS_FS, .interval = 0 },
    },
    .ep_in_count = 1,
    .ep_out_req = { { 0 } },
    .ep_out_count = 0,                /* 视频**只有 IN** */
    .ep_out_same_num_as_in = {
        [0 ... USB_COMPOSITE_MAX_EP_PER_DIR - 1] = USB_EP_NO_PAIR,
    },
    .is_vendor = true,
    .build_desc = video_build_desc,
    .init = video_init,
    .on_out = NULL,
    .on_mount = NULL,
    .on_control = NULL,
    .user_ctx = NULL,
    .wcid = true,                     /* T2：视频接口也要 WinUSB，否则主机打不开 EP3 */
    .wcid_compatible_id = UV_WCID_COMPATIBLE_ID,
    .wcid_device_interface_guid = UV_WCID_GUID,
};
