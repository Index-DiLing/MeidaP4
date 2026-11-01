// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · **L2 桥接降级模式**（仅 CONFIG_APP_USBNET_MODE_BRIDGE=y 时编译）
//
// ⚠️ 这是 plan §0.1 的**降级备选**，不是定案路径，而且**未在真机验证过**。
//    在切到它之前请先读下面三条已知问题：
//
//   1) **Wi-Fi STA 在 3 地址模式下无法被透明二层桥接** —— IDF 官方 bridge 示例的
//      README 写得很直白："Only Ethernet and WiFi AP interfaces can be bridged using
//      LwIP bridge since ESP WiFi station will only receive packets destined to it due to
//      operating in 3 address system."（examples/network/bridge/README.md:164）
//      esp_hosted 的 `sta2eth` 示例是用"把 STA 的 MAC 改成有线口的 MAC"来绕这个限制的，
//      代价是 P4 自己不再是一个独立的上游客户端。
//   2) 上游 AP 会看到 **2 个客户端**（P4 + PC）→ 酒店/企业/门户认证 Wi-Fi 下 PC 上不了网。
//   3) 本路径**牺牲 STA 的 lwIP 上报**：一旦注册 `esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ...)`，
//      WIFI_STA_DEF 的 netif 就收不到帧了（也就不会再自己拿 IP）—— 这正是"透明"的含义。
//
// 实现：USB 侧收到 802.3 帧 → 直接丢给 STA 射频；射频收到的帧 → 直接丢给 USB。
// 不走 lwIP，不做 DHCP server，不做 NAPT。

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"   /* esp_wifi_internal_tx / _reg_rxcb / _free_rx_buffer（私有头，仅 IDF 内可用） */
#include "sdkconfig.h"

#include "tinyusb_net.h"
#include "usb_net_priv.h"

static const char *TAG = "usb_net_br";

static bool s_rx_registered;

/* ---- 射频 → USB ---- */
static esp_err_t bridge_wifi_to_usb(void *buffer, uint16_t len, void *eb)
{
    /* esp_wifi 拥有这块 buffer：交给 esp_tinyusb 异步发出，发完后由
     * tinyusb_net_config_t.free_tx_buffer（= usb_net_tx_free_cb → usb_net_bridge_free_rx_buffer）
     * 调 esp_wifi_internal_free_rx_buffer() 释放。eb 通常就是那个要释放的"外部缓冲"，
     * 与 sta2eth 示例的 `wired_send(buffer, len, eb)` 一致。 */
    void *owner = eb ? eb : buffer;

    esp_err_t err = tinyusb_net_send_async(buffer, len, owner);
    if (err != ESP_OK) {
        usb_net_bridge_free_rx_buffer(owner);
        g_usbnet.rx_dropped++;
        return err;
    }
    g_usbnet.tx_packets++;
    g_usbnet.tx_bytes += len;
    return ESP_OK;
}

/* ---- USB → 射频 ---- */
esp_err_t usb_net_bridge_usb_to_wifi(const void *frame, uint16_t len)
{
    if (!g_usbnet.upstream) {
        g_usbnet.rx_dropped++;
        return ESP_ERR_INVALID_STATE;
    }

    /* 拷一份再发：NCM 的 NTB 缓冲在 rx 回调返回后即失效，而 esp_hosted 的 SDIO 发送
     * 可能不在调用栈内把数据搬完（与原生 Wi-Fi 的 esp_wifi_internal_tx 不同）。 */
    void *copy = malloc(len);
    if (!copy) {
        g_usbnet.rx_dropped++;
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, frame, len);

    int rc = esp_wifi_internal_tx(WIFI_IF_STA, copy, len);
    free(copy);

    if (rc != 0) {
        g_usbnet.rx_dropped++;
        return ESP_FAIL;
    }
    g_usbnet.rx_packets++;
    g_usbnet.rx_bytes += len;
    return ESP_OK;
}

esp_err_t usb_net_bridge_set_upstream(esp_netif_t *upstream)
{
    if (!upstream) {
        return ESP_ERR_INVALID_ARG;
    }
    g_usbnet.upstream = upstream;

    if (s_rx_registered) {
        return ESP_OK;   /* 幂等：契约里"运行中补上/替换上游" */
    }
    esp_err_t err = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, bridge_wifi_to_usb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_internal_reg_rxcb: %s", esp_err_to_name(err));
        return err;
    }
    s_rx_registered = true;
    ESP_LOGW(TAG, "rxcb 已挂到 WIFI_IF_STA：STA 的 netif 从此收不到帧（= 透明桥的代价，"
                  "P4 自己不再是上游客户端）");
    return ESP_OK;
}

esp_err_t usb_net_bridge_init(void)
{
    s_rx_registered = false;
    ESP_LOGW(TAG, "桥接模式：不做 DHCP server、不做 NAPT —— PC 的 IP/网关/DNS 全部由上游 AP 下发");
    ESP_LOGW(TAG, "⚠️ 未验证：Windows 认不认 NCM、STA 侧 L2 桥接能否真的通（3 地址模式限制）");
    return ESP_OK;
}

esp_err_t usb_net_bridge_stop(void)
{
    if (s_rx_registered) {
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        s_rx_registered = false;
    }
    return ESP_OK;
}

void usb_net_bridge_free_rx_buffer(void *buffer)
{
    esp_wifi_internal_free_rx_buffer(buffer);
}
