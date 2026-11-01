// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · **完整版（prod）应用入口**
//
// 只在 `CONFIG_PROD_APP_ENABLE=y` 时定义 `app_main()`；探测入口 `main.cpp` 在同条件下
// 被完全排除（见 `main/CMakeLists.txt`）。两者**互斥**，因为：
//   `components/usb_probe` 与 `components/usb_composite` **各自定义**了强符号
//   `tud_descriptor_bos_cb()` / `tud_vendor_control_xfer_cb()` —— 同时进链接会 multiple definition。
//
// ============================================================================
// 初始化顺序（**这就是本文件存在的全部理由**，每一步都有硬约束）
// ============================================================================
//  1) net_hosted_start()        上游 STA。可失败/可只扫描 —— 不起也不阻塞 USB。
//  2) mtp_backend_start()       **必须第一个注册**（拿 IF0）＋ MTP 类驱动 install
//                               （tinyusb_mtp_install_driver 必须先于 tinyusb_driver_install）
//  3) usb_vendor_register()     两个 vendor 功能：先视频(IF1) 后控制(IF2)
//  4) usb_net_start()           NCM(IF3+IF4) + tinyusb_net_init()（同理必须早于 install）
//  5) video_pipe_start()        I2 帧源；I2 契约要求早于 usb_composite_start()
//  6) usb_vendor_start()        webusb_proto_start() + 视频发送任务（任务自己等 mount）
//  7) usb_composite_start()     **最后**：描述符自检 → tinyusb_driver_install()
//
// ⚠️ 接口号 = **注册顺序**。任何重排都会让 NCM 拿不到 IF3/IF4、或让 MTP 丢失 IF0，
//    而 `usb_composite` 的自检只校验"连续性"，**不会**告诉你"谁应该是 IF3"。
//    所以 2)→3)→4) 的次序是冻结的（docs/contracts/frozen-allocation.md §1）。
//
// ⚠️ 失败策略：**除 usb_composite_start() 之外，任何一步失败都不 abort**。
//    理由（探测期结论，probe-report §2.3）：SD 挂不上 / Wi-Fi 连不上时，
//    USB 那一半仍必须能跑起来，否则连"哪一半坏了"都测不出来。
//
// 写入者：Lead（main/ 是 Lead 的写作用域）。
// 设备验收：docs/device-test-requests.md 的 T1。

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "file_service.h"
#include "mtp_backend.h"
#include "net_hosted.h"
#include "usb_composite.h"
#include "usb_net.h"
#include "usb_vendor.h"
#include "video_pipe.h"

#include "webusb_proto.h"

static const char *TAG = "prod";

/** 汇总各子系统的健康状况，一条 INFO 打出来 —— 真机测试时这就是第一手证据 */
static void print_health(void)
{
    net_hosted_status_t nh;
    net_hosted_get_status(&nh);

    usb_vendor_stats_t vs;
    usb_vendor_get_stats(&vs);

    video_pipe_stats_t vp;
    video_pipe_get_stats(&vp);

    usb_net_stats_t ns;
    usb_net_get_stats(&ns);

    usb_composite_stats_t cs;
    usb_composite_get_stats(&cs);

    ESP_LOGI(TAG, "-----------------------------------------------------------");
    ESP_LOGI(TAG, "USB      : mounted=%d mount=%" PRIu32 " unmount=%" PRIu32
                  " ctrl=%" PRIu32 " stall=%" PRIu32,
             (int)usb_composite_is_mounted(), cs.mount_count, cs.unmount_count,
             cs.ctrl_requests, cs.ctrl_stalls);
    // ⚠️ T1/U8 关键判据：Windows 必须发过 MS OS 2.0 请求（bRequest=0x02）；
    //    而 webusb_get_url 应当恒为 0（我们 iLandingPage=0，Chrome 不该来取着陆页）。
    ESP_LOGI(TAG, "WCID     : msos_req=%" PRIu32 "  webusb_get_url=%" PRIu32 " (期望 >0 / ==0)",
             cs.msos_requests, cs.webusb_get_url);
    ESP_LOGI(TAG, "MTP      : driver=%d storage=%d base=%s",
             (int)mtp_backend_is_driver_installed(), (int)mtp_backend_is_storage_registered(),
             mtp_backend_mount_point());
    {
        file_service_stats_t fs;
        file_service_get_stats(&fs);
        ESP_LOGI(TAG, "FILE     : req=%" PRIu32 " rsp=%" PRIu32 " rd=%" PRIu32 " wr=%" PRIu32
                      " err=%" PRIu32 " open=%" PRIu32 " pages=%" PRIu32 " share_hits=%" PRIu32,
                 fs.requests, fs.responses, fs.bytes_read, fs.bytes_written,
                 fs.errors, fs.handles_open, fs.list_pages, fs.share_probe_hits);
    }
    ESP_LOGI(TAG, "Vendor   : ep_bound=%d frames_sent=%" PRIu32 " dropped=%" PRIu32
                  " bytes=%" PRIu32 " stale_skip=%" PRIu32,
             (int)vs.endpoints_bound, vs.video_frames_sent, vs.video_frames_dropped,
             vs.video_bytes_sent, vs.video_stale_skips);
    {
        /* 线协议服务的真机状态：HELLO 发了几次 / 有没有 link_up / 最近一次收到主机帧多久前 */
        webusb_proto_stats_t ws;
        webusb_proto_get_stats(&ws);
        ESP_LOGI(TAG, "Proto    : link_up=%d hello_sent=%" PRIu32 " hello_ack=%" PRIu32
                      " frames_tx=%" PRIu32 " frames_rx=%" PRIu32 " in=%" PRIu32
                      " out=%" PRIu32 " gen=%" PRIu32 " since_rx=%" PRIu32 "ms",
                 (int)ws.link_up, ws.helloSent, ws.helloAckRecv, ws.frames_tx, ws.frames_rx,
                 ws.in_bytes, ws.out_bytes, ws.link_generation, ws.since_rx_ms);
    }
    ESP_LOGI(TAG, "Video    : frames=%" PRIu32 " dropped=%" PRIu32 " fps_x100=%" PRIu32
                  " kbps=%" PRIu32 " bytes_avg=%" PRIu32 " enc_us=%" PRIu32 "/%" PRIu32,
             vp.frames_out, vp.frames_dropped, vp.fps_actual_x100, vp.kbps,
             vp.bytes_avg, vp.encode_us_avg, vp.encode_us_max);
    ESP_LOGI(TAG, "Upstream : %s connected=%d has_ip=%d dns=" IPSTR " disc=%" PRIu32,
             net_hosted_state_str(nh.state), (int)nh.connected, (int)nh.has_ip,
             IP2STR(&nh.dns), nh.disconnects);
    ESP_LOGI(TAG, "USB net  : up=%d upstream_up=%d clients=%" PRIu32
                  " rx=%" PRIu32 "/%" PRIu32 " tx=%" PRIu32 "/%" PRIu32
                  " drop=%" PRIu32 "/%" PRIu32 " nat=%" PRIu32,
             (int)ns.up, (int)ns.upstream_up, ns.clients,
             ns.rx_packets, ns.rx_bytes, ns.tx_packets, ns.tx_bytes,
             ns.rx_dropped, ns.tx_dropped, ns.nat_entries);
    ESP_LOGI(TAG, "C6       : ver_valid=%d target=%s fw=%s rev=%" PRId32 " chip=0x%08" PRIx32,
             (int)nh.cp_ver_valid, nh.cp_target, nh.cp_fw_version, nh.cp_fw_revision,
             nh.cp_chip_id);
    ESP_LOGI(TAG, "Heap     : internal=%u psram_free=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "-----------------------------------------------------------");
}

static void health_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        print_health();
    }
}

/** 顺序关键、但"失败了也要继续"的一步：统一成这个 helper，避免某处漏掉不-abort 策略。 */
static void try_step(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s : OK", what);
    } else {
        ESP_LOGE(TAG, "%s : FAILED (%s) -- 继续，USB 那一半仍要能跑", what, esp_err_to_name(err));
    }
}

/** 十六进制 dump 一段描述符（真机枚举失败时这是**唯一**能拿到"Windows 到底看到什么"的手段） */
static void hexdump(const char *what, const uint8_t *p, uint32_t len)
{
    if (p == NULL || len == 0) {
        ESP_LOGW(TAG, "%s: (空)", what);
        return;
    }
    ESP_LOGI(TAG, "%s: %" PRIu32 " B", what, len);
    char line[64];
    for (uint32_t i = 0; i < len; i += 16) {
        int n = 0;
        for (uint32_t j = i; j < i + 16 && j < len; j++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%02X ", p[j]);
        }
        ESP_LOGI(TAG, "  [%3" PRIu32 "] %s", i, line);
    }
}

/** 把配置描述符按描述符类型逐条列出（比裸 hex 更容易看出结构问题） */
static void walk_cfg(const char *tag_name, bool hs)
{
    uint32_t len = 0;
    const uint8_t *p = usb_composite_config_desc(hs, &len);
    if (p == NULL || len == 0) {
        return;
    }
    ESP_LOGI(TAG, "%s 结构：total=%" PRIu32 " numItf=%u", tag_name, len, p[4]);
    uint32_t off = 0;
    while (off + 2 <= len) {
        uint8_t dlen = p[off], dtype = p[off + 1];
        if (dlen < 2 || off + dlen > len) {
            ESP_LOGE(TAG, "  !! 偏移 %" PRIu32 " bLength=%u 越界", off, dlen);
            break;
        }
        switch (dtype) {
        case 0x04:  /* interface */
            ESP_LOGI(TAG, "  itf num=%u alt=%u nEP=%u class=%02X/%02X/%02X iStr=%u",
                     p[off + 2], p[off + 3], p[off + 4], p[off + 5], p[off + 6], p[off + 7], p[off + 8]);
            break;
        case 0x05:  /* endpoint */
            ESP_LOGI(TAG, "    ep  addr=0x%02X attr=%02X mps=%u interval=%u",
                     p[off + 2], p[off + 3],
                     (unsigned)(p[off + 4] | (p[off + 5] << 8)), p[off + 6]);
            break;
        case 0x0B:  /* IAD */
            ESP_LOGI(TAG, "  IAD first=%u count=%u class=%02X/%02X/%02X",
                     p[off + 2], p[off + 3], p[off + 4], p[off + 5], p[off + 6]);
            break;
        case 0x24:  /* CS_INTERFACE (CDC class-specific) */
            ESP_LOGI(TAG, "    cs  subtype=%02X len=%u", p[off + 2], dlen);
            break;
        default:
            ESP_LOGI(TAG, "  ??? type=0x%02X len=%u", dtype, dlen);
            break;
        }
        off += dlen;
    }
    if (off != len) {
        ESP_LOGE(TAG, "  !! 解析到 %" PRIu32 " 与 total=%" PRIu32 " 不符", off, len);
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "===========================================================");
    ESP_LOGI(TAG, "MeidaP4 **COMPOSITE** firmware | IDF %s | target %s",
             esp_get_idf_version(), CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "PSRAM    : total=%u B free=%u B",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "internal : total=%u B free=%u B",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "===========================================================");

    // ---- 1) 上游（Wi-Fi STA via C6）。可只扫描、可失败，均不阻塞 USB ----
    try_step("1 net_hosted_start", net_hosted_start());

    // ---- 2) MTP：**必须第一个注册**（IF0）----
    try_step("2 mtp_backend_start", mtp_backend_start());

    // ---- 2.5) FILE 通道服务（TF 卡共享）：SD 已在 2) 里挂载 ----
    //     注册 handler ⇒ HELLO 的 caps 自动带上 FILE 位；必须在 usb_composite_start() 之前。
    try_step("2.5 file_service_start", file_service_start());

    // ---- 3) vendor 双功能：视频(IF1) → 控制(IF2) ----
    try_step("3 usb_vendor_register", usb_vendor_register());

    // ---- 4) NCM：IF3+IF4（最后注册）----
    {
        usb_net_config_t nc;
        usb_net_config_default(&nc);
        nc.upstream_netif = net_hosted_upstream_netif();  // 可为 NULL，之后 set_upstream 补
        try_step("4 usb_net_start", usb_net_start(&nc));
    }

    // ---- 5) 视频管线（占位实现也要起来，否则 video_pipe_get_frame 恒 INVALID_STATE）----
    {
        video_pipe_config_t vc;
        video_pipe_config_default(&vc);
        try_step("5 video_pipe_start", video_pipe_start(&vc));
    }

    // ---- 6) 协议服务 + 视频发送任务 ----
    try_step("6 usb_vendor_start", usb_vendor_start());

    // ---- 7) 复合层：自检 → tinyusb_driver_install。**这一步失败就真的没 USB 了** ----
    esp_err_t err = usb_composite_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "7 usb_composite_start : FAILED (%s) -- 无 USB 复合设备",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "7 usb_composite_start : OK —— 枚举已就绪，等主机");
    }

    // ---- 上线后把冻结表里最关键的几个数字打出来，供真机对照 ----
    usb_composite_budget_t b;
    if (usb_composite_get_budget(&b) == ESP_OK) {
        ESP_LOGI(TAG, "预算 | IF=%u FN=%u(vendor %u) IN=%u OUT=%u | FS=%" PRIu32
                      " HS=%" PRIu32 " BOS=%" PRIu32 " MSOS=%" PRIu32 " str=%" PRIu32,
                 b.itf_total, b.fn_total, b.vendor_fn_total, b.ep_in_total, b.ep_out_total,
                 b.fs_cfg_len, b.hs_cfg_len, b.bos_len, b.msos_len, b.str_count);
        ESP_LOGI(TAG, "预算 | DWC2 FIFO used=%u / %u word (free %u)",
                 b.fifo_words_used, b.fifo_words_total, b.fifo_words_free);
        // 冻结期望（frozen-allocation §1/§2）：IF=5 FN=4 vendor=2 IN=6 OUT=3；FIFO 884/1024
        if (b.itf_total != 5 || b.fn_total != 4 || b.ep_in_total != 6 || b.ep_out_total != 3) {
            ESP_LOGE(TAG, "!! 预算与冻结表不符：期望 IF=5 FN=4 IN=6 OUT=3 —— 注册顺序可能错了");
        }
    }

    xTaskCreate(health_task, "prod_health", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "健康遥测任务已启动（10 s 周期）");

    // ---- 描述符落档：枚举失败时靠这段对照规范（真机调试的第一手证据）----
    {
        uint32_t dlen = 0, blen = 0, mlen = 0;
        hexdump("HS 配置描述符", usb_composite_config_desc(true, &dlen), dlen);
        walk_cfg("HS", true);
        hexdump("FS 配置描述符", usb_composite_config_desc(false, &dlen), dlen);
        walk_cfg("FS", false);
        hexdump("BOS", usb_composite_bos_desc(&blen), blen);
        hexdump("MS OS 2.0 集", usb_composite_msos_desc(&mlen), mlen);
        hexdump("Device 描述符", usb_composite_device_desc(&dlen), dlen);
    }
}
