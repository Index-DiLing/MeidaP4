// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 探测固件入口
//
// ⚠️ 本文件是**接线层**，只做三件事：先 MTP 后 USB 的初始化顺序、启动横幅、周期性遥测。
//    描述符与协议逻辑放在 components/usb_probe 与 components/mtp_probe，不要写到这里。
//    写入者：Lead（其他人不要改）。

#include <cinttypes>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mtp_probe.h"
#include "usb_probe.h"

static const char *TAG = "probe";

#if CONFIG_TINYUSB_MTP_ENABLED
#define PROBE_VENDOR_ITF PROBE_ITF_VENDOR_B
#else
#define PROBE_VENDOR_ITF PROBE_ITF_VENDOR_A
#endif

static void telemetry_task(void *arg)
{
    (void)arg;
    usb_probe_stats_t st;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        usb_probe_get_stats(&st);
        ESP_LOGI(TAG,
                 "stats | mounted=%d out=%" PRIu32 "B/%" PRIu32 "pkt in=%" PRIu32 "B/%" PRIu32
                 "pkt ctrl=%" PRIu32 " stall=%" PRIu32 " | heap=%u psram_free=%u",
                 (int)st.mounted, st.out_bytes, st.out_packets, st.in_bytes, st.in_packets,
                 st.ctrl_requests, st.ctrl_stalls, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

// ⚠️ 本文件与 main_prod.cpp **互斥**：由 main/CMakeLists.txt 依据 build.ps1 传入的
//    `-DMEIDAP4_MODE` 决定哪个进 SRCS（**不用** Kconfig 值做结构判断，理由见该文件顶部）。
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, "MeidaP4 USB probe | IDF %s | target %s", esp_get_idf_version(), CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "probe mode : %s", CONFIG_PROBE_MODE_NAME);
    ESP_LOGI(TAG, "PSRAM      : total=%u B  free=%u B",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "internal   : total=%u B  free=%u B",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "=====================================================");

    // 初始化失败**不** abort：探测固件要尽量把能测的那一半测完。
    // 用 ESP_ERROR_CHECK 会在 MTP 挂载失败时把 mode B 打成 boot loop，连 USB/WebUSB 那一半
    // 都没法探测（mtp-core 提出，2026-10 采纳）。

#if CONFIG_PROBE_ENABLE_MTP
    // 顺序要求：MTP 类驱动必须先 install，TinyUSB 才能后装（见 probe-contract §3.2）
    esp_err_t mtp_err = mtp_probe_start();
    if (mtp_err == ESP_OK) {
        ESP_LOGI(TAG, "MTP volume : %s (mounted=%d)", mtp_probe_mount_point(), (int)mtp_probe_is_mounted());
    } else {
        ESP_LOGE(TAG, "MTP volume : FAILED (%s) -- continuing so the USB half is still probeable",
                 esp_err_to_name(mtp_err));
    }
#else
    ESP_LOGI(TAG, "MTP volume : disabled (mode A)");
#endif

#if CONFIG_PROBE_ENABLE_USB
    esp_err_t usb_err = usb_probe_start();
    if (usb_err == ESP_OK) {
        ESP_LOGI(TAG, "USB probe  : started | vendor itf=%d | HS cfg len=%" PRIu32,
                 PROBE_VENDOR_ITF, usb_probe_config_desc_len(true));
    } else {
        ESP_LOGE(TAG, "USB probe  : FAILED (%s) -- nothing USB to test in this boot",
                 esp_err_to_name(usb_err));
    }
#else
    ESP_LOGE(TAG, "CONFIG_PROBE_ENABLE_USB=n -- 探测固件没有 USB，什么都测不了");
#endif

    xTaskCreate(telemetry_task, "probe_telem", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "telemetry task started (5 s period)");
}
