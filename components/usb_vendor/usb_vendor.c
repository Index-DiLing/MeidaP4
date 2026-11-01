// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · WS-webusb 的两个 vendor 功能 —— 注册与启动
//
// 注册顺序**固定**：先 IF1 视频、后 IF2 控制（接口号 = 注册顺序，frozen-allocation §1）。
// 调用方（main）必须按这个顺序把本工作流插入整条链：
//     usb_composite_register(MTP)            → IF0
//     usb_vendor_register()                  → IF1 视频 + IF2 控制   ← 本文件
//     usb_composite_register(NCM)            → IF3 + IF4
//     usb_vendor_start() / usb_net_start() / ...  （在 usb_composite_start() 之前）
//     usb_composite_start()

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "usb_composite.h"

#include "usb_vendor.h"
#include "usb_vendor_priv.h"
#include "webusb_proto.h"

static const char *TAG = "usb_vendor";

usb_vendor_stats_t g_uv_stats;

static bool s_registered;

esp_err_t usb_vendor_register(void)
{
    esp_err_t err;

    if (s_registered) {
        return ESP_OK;
    }

    /* 顺序即接口号：视频 = IF1，控制 = IF2（也都是 vendor 驱动里的 idx 0/1） */
    err = usb_composite_register(&usb_vendor_fn_video);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IF1(视频) 功能失败: %s", esp_err_to_name(err));
        return err;
    }
    err = usb_composite_register(&usb_vendor_fn_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IF2(控制) 功能失败: %s", esp_err_to_name(err));
        return err;
    }

    s_registered = true;
    ESP_LOGI(TAG, "已注册 2 个 vendor 功能：IF1 视频（只有 IN）/ IF2 控制（IN+OUT，WCID=WINUSB）");
    return ESP_OK;
}

bool usb_vendor_registered(void)
{
    return s_registered;
}

esp_err_t usb_vendor_start(void)
{
    esp_err_t err;

    /* 协议服务（I0）先起来：它只建队列/任务，不碰 USB 硬件；
       IF2 的端点号要等 usb_composite 回调 init(alloc) 才拿得到。 */
    err = webusb_proto_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "webusb_proto_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 视频发送任务（等 mount 后自己取帧；video_pipe 未落地时降级为空转） */
    err = uv_video_task_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "视频任务启动失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void usb_vendor_get_stats(usb_vendor_stats_t *out)
{
    webusb_proto_stats_t ps;

    if (out == NULL) {
        return;
    }
    *out = g_uv_stats;
    out->endpoints_bound = uv_ctrl_bound();
    /* OUT 字节数的权威计数在 I0 服务里（on_out 排空时累计），这里取回来，避免两处口径不一 */
    webusb_proto_get_stats(&ps);
    out->ctrl_out_bytes = ps.out_bytes;
}
