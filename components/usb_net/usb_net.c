// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I3 契约实现 —— **USB 网卡（NCM）↔ esp_netif 桥接**
//
// 架构定案（plan §0.1）：**NAPT 路由**，不做 L2 桥接。PC 落在 P4 后面的私网
// （默认 192.168.77.0/24，P4 = .1），P4 跑 DHCP server，用 lwIP NAPT 把 PC 的流量 NAT 出去、
// 经 Wi-Fi STA（C6 射频，见 components/net_hosted）上网。
//
// 三条决定性理由（plan §0.1 原文）：① 上游 AP 只看到 1 个客户端 → 酒店/企业/访客 Wi-Fi 也能用；
// ② 避免 PC 出现"同网段双路由"这类极难排查的间歇故障；③ Espressif 在同一套栈（hosted + NAPT）
// 里已跑通（`examples/wifi/softap_sta` 的 `esp_netif_napt_enable()`）。
//
// 本文件里几个**容易踩错、离线看不出来**的点，逐条写在对应代码旁边：
//   1) NAPT 开在**内侧**（USB netif），不是上游 —— lwIP 的 `ip_napt_forward()` 只看 `inp->napt`
//      （lwip/src/core/ipv4/ip4_napt.c:868-872），`inp` 就是收包的那个接口。softap_sta 示例
//      也是 `esp_netif_napt_enable(ap)`。
//   2) USB netif 上**关掉 IPv6**（RA/SLAAC/DHCPv6）——但不碰全局 `CONFIG_LWIP_IPV6`，
//      免得波及 esp_hosted 的上游。见 usb_net_assert_ipv6_off()。
//   3) **DNS 不做代理**：DHCP option 6 直接把上游 STA 拿到的 DNS 透传下去（`esp_netif_get_dns_info`），
//      改 option 必须先停 DHCP server（apsta 示例的姿势）。
//   4) DHCP server 的**网关**取 netif 的 `ip_info.gw`（dhcpserver.c:466-476），所以 gw 必须自己填。
//   5) `tinyusb_net_send_sync()` 用 `usbd_defer_func` + 事件组等 TinyUSB 任务把数据 memcpy 走，
//      所以只能从**非 TinyUSB 任务**调用（lwIP tcpip 线程 OK）；且 esp_tinyusb 那份实现是
//      **不可重入**的单例（s_net_obj.packet_to_send 一个全局），这里用 tx_lock 串行化。
//
// 冻结契约：include/usb_net.h（**不得修改**）。端点/接口号全部由 I1 分配（见 usb_net_ncm.c）。

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "dhcpserver/dhcpserver.h"   /* dhcps_lease_t / OFFER_DNS（IDF 里没有 DHCPS_OFFER_DNS，
                                        softap_sta 示例是自己 #define 的） */
#include "lwip/esp_netif_net_stack.h"
#include "sdkconfig.h"

#if CONFIG_TINYUSB_NET_MODE_NCM || CONFIG_TINYUSB_NET_MODE_ECM_RNDIS
#include "tinyusb_net.h"
#define USB_NET_HAVE_TINYUSB_NET 1
#else
#define USB_NET_HAVE_TINYUSB_NET 0
#endif

#if CONFIG_LWIP_STATS && CONFIG_LWIP_IPV4_NAPT
#include "lwip/lwip_napt.h"
#include "lwip/stats.h"
#define USB_NET_HAVE_NAPT_STATS 1
#else
#define USB_NET_HAVE_NAPT_STATS 0
#endif

#include "usb_net.h"
#include "usb_net_priv.h"

static const char *TAG = "usb_net";

/* 默认值（usb_net.h 的注释：NAPT / MAC 随 STA / 192.168.77.1/24 / DNS 透传） */
#define USB_NET_DEFAULT_IP      "192.168.77.1"
#define USB_NET_DEFAULT_MASK    "255.255.255.0"
#define USB_NET_DEFAULT_START   "192.168.77.2"

usb_net_ctx_t g_usbnet;

/* esp_netif_new() 会拷贝 ip_info 的**内容**，但 base/driver/stack 三个结构体在
 * post_attach 之前一直被读（esp_netif_lwip.c:682-786），所以放文件作用域，避免悬垂。 */
static esp_netif_ip_info_t          s_ip_info;
static esp_netif_inherent_config_t  s_base_cfg;
static esp_netif_driver_ifconfig_t  s_driver_cfg;
static struct esp_netif_netstack_config s_lwip_cfg;

uint32_t usb_net_ip4_host(const esp_ip4_addr_t *a)
{
    /* esp_ip4_addr_t.addr 是网络字节序；esp_netif_htonl 在 LE 上是字节翻转且**自反** */
    return esp_netif_htonl(a->addr);
}

void usb_net_ip4_set_host(esp_ip4_addr_t *a, uint32_t h)
{
    a->addr = esp_netif_htonl(h);
}

static void usb_net_mac_to_str(const uint8_t mac[6], char out[USB_NET_MAC_STR_LEN])
{
    snprintf(out, USB_NET_MAC_STR_LEN, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool usb_net_mac_is_zero(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            return false;
        }
    }
    return true;
}

/* =====================================================================
 * 数据面：USB → lwIP
 * ===================================================================== */

/**
 * esp_tinyusb 的 net 类 rx 回调（在 **TinyUSB 任务**上下文，不可阻塞）。
 * 给进来的是 NCM 驱动内部的 NTB 缓冲，回调返回后即失效 → 必须拷一份交给 lwIP。
 * 所有权：`ethernetif_input()` 在所有分支上都会通过 driver_free_rx_buffer 释放它
 * （esp_netif/lwip/netif/ethernetif.c:119-146），所以这里不要自己 free。
 */
#if USB_NET_HAVE_TINYUSB_NET
static esp_err_t usb_net_usb_rx_cb(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;

#if CONFIG_APP_USBNET_MODE_BRIDGE
    return usb_net_bridge_usb_to_wifi(buffer, len);
#else
    if (!g_usbnet.netif) {
        g_usbnet.rx_dropped++;
        return ESP_ERR_INVALID_STATE;
    }
    void *copy = malloc(len);
    if (!copy) {
        g_usbnet.rx_dropped++;
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    g_usbnet.rx_packets++;
    g_usbnet.rx_bytes += len;
    if (len > USB_NET_NCM_SEG_SIZE) {
        /* 不该发生（NCM 驱动按 MTU 切包）；记录下来便于排查 */
        ESP_LOGW(TAG, "rx 帧 %u B 超过段大小 %u B", (unsigned)len, (unsigned)USB_NET_NCM_SEG_SIZE);
    }
    esp_err_t err = esp_netif_receive(g_usbnet.netif, copy, len, NULL);
    if (err != ESP_OK) {
        /* buffer 已被 ethernetif_input() 释放（任何分支都会释放） */
        ESP_LOGD(TAG, "esp_netif_receive: %s（帧已丢弃）", esp_err_to_name(err));
    }
    return err;
#endif
}
#endif  /* USB_NET_HAVE_TINYUSB_NET */

/* =====================================================================
 * 数据面：lwIP → USB
 * ===================================================================== */

/* ⚠️ NCM=n 时本回调没有任何引用点 → 不连着 rx_cb 一起编译掉会触发
 *    -Werror=unused-function（IDF 对工程组件默认 -Wall -Werror -Wextra）。 */
#if USB_NET_HAVE_TINYUSB_NET
/** 异步发送路径的 buffer 释放回调（NAPT 模式下用不到：我们只走 send_sync） */
static void usb_net_tx_free_cb(void *buffer, void *ctx)
{
    (void)ctx;
#if CONFIG_APP_USBNET_MODE_BRIDGE
    usb_net_bridge_free_rx_buffer(buffer);
#else
    free(buffer);
#endif
}
#endif  /* USB_NET_HAVE_TINYUSB_NET */

esp_err_t usb_net_lwip_to_usb(void *buffer, uint16_t len)
{
    /* NCM=n 时 buffer 用不到 → 提前吃掉，避免 -Werror=unused-parameter */
    (void)buffer;
    if (!g_usbnet.started) {
        return ESP_ERR_INVALID_STATE;
    }
#if USB_NET_HAVE_TINYUSB_NET
    /* 同步发送：内容由 TinyUSB 任务 memcpy 进 NTB，返回时 lwIP 的 pbuf 仍然有效。
     * ⚠️ 不能用 send_async —— lwIP 在 output() 返回后立刻释放 pbuf，异步会在 USB 侧读到野指针。 */
    if (g_usbnet.tx_lock && xSemaphoreTake((SemaphoreHandle_t)g_usbnet.tx_lock,
                                           pdMS_TO_TICKS(CONFIG_APP_USBNET_TX_LOCK_MS)) != pdTRUE) {
        g_usbnet.tx_dropped++;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = tinyusb_net_send_sync(buffer, len, NULL,
                                          pdMS_TO_TICKS(CONFIG_APP_USBNET_TX_TIMEOUT_MS));
    if (g_usbnet.tx_lock) {
        xSemaphoreGive((SemaphoreHandle_t)g_usbnet.tx_lock);
    }
#else
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err == ESP_OK) {
        g_usbnet.tx_packets++;
        g_usbnet.tx_bytes += len;
    } else {
        g_usbnet.tx_dropped++;
    }
    return err;
}

/** esp_netif 的 transmit 回调：buffer 的生命周期由 lwIP 拥有，返回后它会自己释放 */
static esp_err_t usb_net_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (len > UINT16_MAX) {
        g_usbnet.tx_dropped++;
        return ESP_ERR_INVALID_SIZE;
    }
    return usb_net_lwip_to_usb(buffer, (uint16_t)len);
}

/** esp_netif 的 driver_free_rx_buffer：释放我们交给 lwIP 的那份拷贝 */
static void usb_net_netif_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* =====================================================================
 * 查询
 * ===================================================================== */

esp_netif_t *usb_net_get_netif(void)
{
    return g_usbnet.started ? g_usbnet.netif : NULL;
}

uint32_t usb_net_client_count(void)
{
    return g_usbnet.client_count;
}

void usb_net_get_stats(usb_net_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->up = g_usbnet.started && (g_usbnet.netif != NULL);

    if (g_usbnet.upstream) {
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        if (esp_netif_get_ip_info(g_usbnet.upstream, &info) == ESP_OK && info.ip.addr != 0) {
            out->upstream_up = true;
        }
    }

    out->rx_packets = g_usbnet.rx_packets;
    out->tx_packets = g_usbnet.tx_packets;
    out->rx_bytes = g_usbnet.rx_bytes;
    out->tx_bytes = g_usbnet.tx_bytes;
    out->rx_dropped = g_usbnet.rx_dropped;
    out->tx_dropped = g_usbnet.tx_dropped;
    out->clients = g_usbnet.client_count;

#if USB_NET_HAVE_NAPT_STATS
    struct stats_ip_napt ns;
    memset(&ns, 0, sizeof(ns));
    ip_napt_get_stats(&ns);
    out->nat_entries = (uint32_t)ns.nr_active_tcp + (uint32_t)ns.nr_active_udp + (uint32_t)ns.nr_active_icmp;
#else
    /* ⚠️ 未撒谎：lwIP 只在 `CONFIG_LWIP_STATS=y` 时才暴露 NAPT 会话表（ip_napt_get_stats，
     *  ip4_napt.c:1081-1087 被 #if LWIP_STATS 包着）。默认 prod 配置是 n → 这里恒为 0。
     *  想让这个字段有值：在 sdkconfig.prod.defaults 加 `CONFIG_LWIP_STATS=y`（会增加少量代码尺寸）。 */
    out->nat_entries = 0;
#endif
}

/* =====================================================================
 * DHCP 客户端统计（IP_EVENT_ASSIGNED_IP_TO_CLIENT）
 * ===================================================================== */

static void usb_net_add_client(const uint8_t mac[6], const esp_ip4_addr_t *ip)
{
    for (uint8_t i = 0; i < g_usbnet.client_count; i++) {
        if (memcmp(g_usbnet.clients[i].mac, mac, 6) == 0) {
            g_usbnet.clients[i].ip = *ip;
            return;
        }
    }
    if (g_usbnet.client_count < USB_NET_MAX_CLIENTS) {
        memcpy(g_usbnet.clients[g_usbnet.client_count].mac, mac, 6);
        g_usbnet.clients[g_usbnet.client_count].ip = *ip;
        g_usbnet.client_count++;
        ESP_LOGI(TAG, "USB 侧客户端 #%u: " IPSTR " (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
                 (unsigned)g_usbnet.client_count, IP2STR(ip),
                 (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                 (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    } else {
        ESP_LOGW(TAG, "DHCP 客户端表满（%d）；clients 计数不再增长（表只用于统计）", USB_NET_MAX_CLIENTS);
    }
}

/* =====================================================================
 * DNS 透传（不做代理）
 * ===================================================================== */

void usb_net_apply_dns_from_upstream(void)
{
#if CONFIG_APP_USBNET_MODE_BRIDGE
    /* 桥接模式：PC 直接用上游 AP 的 DHCP，option 6 由上游下发，这里什么都不做 */
    return;
#else
    if (!g_usbnet.started || !g_usbnet.netif) {
        return;
    }

    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));

    if (g_usbnet.dns_override[0] != '\0') {
        if (esp_netif_str_to_ip4(g_usbnet.dns_override, &dns.ip.u_addr.ip4) != ESP_OK) {
            ESP_LOGE(TAG, "dns_override=\"%s\" 不是合法 IPv4", g_usbnet.dns_override);
            return;
        }
        dns.ip.type = ESP_IPADDR_TYPE_V4;
    } else {
        if (!g_usbnet.upstream) {
            ESP_LOGW(TAG, "还没有上游 netif（net_hosted 未起？）→ DNS 暂不下发，等 IP_EVENT 再刷");
            return;
        }
        esp_err_t gerr = esp_netif_get_dns_info(g_usbnet.upstream, ESP_NETIF_DNS_MAIN, &dns);
        if (gerr != ESP_OK) {
            ESP_LOGW(TAG, "上游 DNS 读取失败: %s", esp_err_to_name(gerr));
            return;
        }
        if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
            ESP_LOGW(TAG, "上游还没有 IPv4 DNS（STA 还没拿到租约）→ 等下次 IP_EVENT");
            return;
        }
    }

    /* ⚠️ 改 DHCP server 的 option 必须先停 server（apsta 示例 softap_set_dns_addr 的姿势，
     *    esp_netif_lwip.c:2631-2633 在 server STARTED 时直接拒绝 ESP_NETIF_OP_SET）。 */
    esp_err_t err = esp_netif_dhcps_stop(g_usbnet.netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED &&
        err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }

    uint8_t offer_dns = OFFER_DNS;
    err = esp_netif_dhcps_option(g_usbnet.netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dhcps_option(DOMAIN_NAME_SERVER): %s", esp_err_to_name(err));
    }

    err = esp_netif_set_dns_info(g_usbnet.netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_dns_info: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_start(g_usbnet.netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dhcps_start: %s", esp_err_to_name(err));
        return;
    }

    snprintf(g_usbnet.dns_str, sizeof(g_usbnet.dns_str), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    ESP_LOGI(TAG, "DHCP option 6 (DNS) = %s  ← %s（**不做 DNS 代理**，plan §0.1）",
             g_usbnet.dns_str, (g_usbnet.dns_override[0] != '\0') ? "配置覆盖" : "透传上游 STA");
#endif
}

/* =====================================================================
 * 事件
 * ===================================================================== */

static void usb_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base != IP_EVENT) {
        return;
    }

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        if (evt && g_usbnet.upstream && evt->esp_netif != g_usbnet.upstream) {
            return;   /* 不是我们的上游，忽略 */
        }
        ESP_LOGI(TAG, "上游拿到 IP → 刷新 DHCP option 6");
        usb_net_apply_dns_from_upstream();
        return;
    }

    if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *evt = (const ip_event_assigned_ip_to_client_t *)data;
        if (!evt || (g_usbnet.netif && evt->esp_netif != g_usbnet.netif)) {
            return;   /* 只统计 USB 侧 DHCP server 的租约 */
        }
        usb_net_add_client(evt->mac, &evt->ip);
        return;
    }
}

/* =====================================================================
 * IPv6 关闭（plan §0.1 明确点名：否则 PC 拿到 IPv6 没回程路由 → "部分站点卡住"）
 * ===================================================================== */

static void usb_net_assert_ipv6_off(void)
{
    if (!g_usbnet.netif) {
        return;
    }
#if !CONFIG_APP_USBNET_MODE_BRIDGE
    /* 我们**没有**给 netif 置 ESP_NETIF_FLAG_IPV6_AUTOCONFIG_ENABLED：
     *   esp_netif_lwip.c:1238-1240 → lwip_netif->ip6_autoconfig_enabled = 0
     *   → 不发 RA、不发 RIO/RDNSS，也不做 SLAAC。
     * 我们也**从不**调 esp_netif_create_ip6_linklocal() → 该接口上没有 IPv6 地址。
     * ESP-IDF 的 esp_netif 里根本没有 DHCPv6 服务端（只有 IPv4 DHCP server），
     * 所以 Windows 最多只拿到自己的 fe80:: 链路本地地址，不会出现"有 v6 地址没回程"。
     *
     * 兜底检查：万一将来有人给这个 netif 加上了 IPv6 地址，这里会当场喊出来。 */
    esp_ip6_addr_t addr6;
    if (esp_netif_get_ip6_linklocal(g_usbnet.netif, &addr6) == ESP_OK) {
        ESP_LOGW(TAG, "⚠️ USB netif 上竟然有 IPv6 地址 —— 与 plan §0.1 冲突（PC 可能\"部分站点卡住\"）");
    }
#endif
#if CONFIG_LWIP_IPV6_FORWARD
    ESP_LOGW(TAG, "⚠️ CONFIG_LWIP_IPV6_FORWARD=y：NAPT 只管 IPv4，v6 转发会让 PC 半通；建议 n");
#endif
    /* ⚠️ bool 在 =n 时 sdkconfig.h 里**没有**宏，所以不能在 C 表达式里直接用 CONFIG_LWIP_IPV6
     *    （会是 "undeclared"）；这里先收敛成宏。 */
#if CONFIG_LWIP_IPV6
#define USB_NET_LWIP_IPV6_ON 1
#else
#define USB_NET_LWIP_IPV6_ON 0
#endif
    ESP_LOGI(TAG, "IPv6: USB netif 上已关（无 RA / 无 SLAAC / 无 DHCPv6；全局 CONFIG_LWIP_IPV6=%d 未动，"
                  "因此 esp_hosted 上游的 IPv6 不受影响）", USB_NET_LWIP_IPV6_ON);
}

/* =====================================================================
 * MAC 解析
 * ===================================================================== */

static void usb_net_resolve_mac(const usb_net_config_t *cfg)
{
    const char *src = NULL;

    if (!usb_net_mac_is_zero(cfg->mac)) {
        memcpy(g_usbnet.mac, cfg->mac, 6);
        src = "cfg.mac 显式给定";
    } else if (cfg->upstream_netif) {
        /* I3 契约：全 0 → 用 STA 的 MAC。直接读上游 netif 缓存的 MAC：
         * 不阻塞、不发 RPC（esp_wifi_get_mac 在 P4 上是 RPC，未握手时会等到超时）。 */
        uint8_t m[6] = { 0 };
        if (esp_netif_get_mac(cfg->upstream_netif, m) == ESP_OK && !usb_net_mac_is_zero(m)) {
            memcpy(g_usbnet.mac, m, 6);
            src = "上游 STA 的 MAC（契约：cfg.mac 全 0）";
        }
    }

    if (!src) {
        /* 退化：上游还没起来（cfg->upstream_netif == NULL）→ 本地管理地址（LAA）。
         * 注意 ESP32-P4 **没有**原生 Wi-Fi，esp_read_mac(ESP_MAC_WIFI_STA) 直接返回
         * ESP_ERR_NOT_SUPPORTED（esp_hw_support/mac_addr.c:363-389 的 case 被 SOC_WIFI_SUPPORTED 包着），
         * 所以从 BASE MAC 派生。 */
        esp_err_t err = esp_read_mac(g_usbnet.mac, ESP_MAC_BASE);
        if (err == ESP_OK) {
            g_usbnet.mac[0] |= 0x02;    /* 本地管理位 */
            g_usbnet.mac[5] ^= 0x01;    /* 与 BASE/STA 区分开 */
            usb_net_mac_to_str(g_usbnet.mac, g_usbnet.mac_str);
            ESP_LOGW(TAG, "USB 网卡 MAC = %s（上游未就绪 → BASE 派生的本地管理地址）。"
                          "建议 net_hosted_start() 之后再 usb_net_start()，或在 cfg.mac 里显式给",
                     g_usbnet.mac_str);
        } else {
            const uint8_t fallback[6] = { 0x02, 0x4D, 0x50, 0x34, 0x4E, 0x01 };
            memcpy(g_usbnet.mac, fallback, 6);
            ESP_LOGW(TAG, "USB 网卡 MAC = %s（连 BASE MAC 都读不到: %s）",
                     g_usbnet.mac_str, esp_err_to_name(err));
        }
        usb_net_mac_to_str(g_usbnet.mac, g_usbnet.mac_str);
        return;
    }

    usb_net_mac_to_str(g_usbnet.mac, g_usbnet.mac_str);
    ESP_LOGI(TAG, "USB 网卡 MAC = %s（%s）", g_usbnet.mac_str, src);
}

/* =====================================================================
 * 生命周期
 * ===================================================================== */

void usb_net_config_default(usb_net_config_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mode = USBNET_MODE_NAPT;
    out->usb_ip = USB_NET_DEFAULT_IP;
    out->usb_netmask = USB_NET_DEFAULT_MASK;
    out->dhcp_start = USB_NET_DEFAULT_START;
    out->dns_override = NULL;      /* NULL = 透传上游 STA 的 DNS（plan §0.1：不做 DNS 代理） */
    out->upstream_netif = NULL;
    out->mtu = 0;                  /* 0 = 默认（NCM wMaxSegmentSize 1514 / L3 MTU 1500） */
    memset(out->mac, 0, sizeof(out->mac));
}

/** 解析网段、校验并算好 DHCP 池 */
static esp_err_t usb_net_prepare_addressing(const usb_net_config_t *cfg,
                                           esp_ip4_addr_t *ip, esp_ip4_addr_t *mask,
                                           esp_ip4_addr_t *pool_start, esp_ip4_addr_t *pool_end,
                                           uint32_t *pool_size)
{
    if (esp_netif_str_to_ip4(cfg->usb_ip, ip) != ESP_OK) {
        ESP_LOGE(TAG, "usb_ip=\"%s\" 不是合法 IPv4", cfg->usb_ip ? cfg->usb_ip : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_netif_str_to_ip4(cfg->usb_netmask, mask) != ESP_OK) {
        ESP_LOGE(TAG, "usb_netmask=\"%s\" 不是合法 IPv4", cfg->usb_netmask ? cfg->usb_netmask : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_netif_str_to_ip4(cfg->dhcp_start, pool_start) != ESP_OK) {
        ESP_LOGE(TAG, "dhcp_start=\"%s\" 不是合法 IPv4", cfg->dhcp_start ? cfg->dhcp_start : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t ip_h = usb_net_ip4_host(ip);
    uint32_t mask_h = usb_net_ip4_host(mask);
    uint32_t net_h = ip_h & mask_h;
    uint32_t bcast_h = net_h | ~mask_h;

    /* 池大小：Kconfig（默认 64）。lwIP 的 DHCP server 硬上限 DHCPS_MAX_LEASE = 100
     * （dhcpserver.h:75；esp_netif_lwip.c:2699 会校验 end-start+1 <= 100）。 */
    uint32_t size = (uint32_t)CONFIG_APP_USBNET_DHCP_POOL_SIZE;
    if (size == 0) {
        size = 1;
    }
    if (size > 100) {
        ESP_LOGW(TAG, "地址池 %" PRIu32 " > lwIP 上限 100（DHCPS_MAX_LEASE），截到 100", size);
        size = 100;
    }

    uint32_t start_h = usb_net_ip4_host(pool_start);

    /* 校验（每条都对应 esp_netif_lwip.c:2665-2701 里的一次拒绝） */
    if (ip_h == net_h || ip_h == bcast_h) {
        ESP_LOGE(TAG, "usb_ip 落在网络号/广播地址上");
        return ESP_ERR_INVALID_ARG;
    }
    if ((ip_h & mask_h) != net_h) {
        ESP_LOGE(TAG, "usb_ip 与掩码不自洽");
        return ESP_ERR_INVALID_ARG;
    }
    if (start_h <= net_h || start_h >= bcast_h) {
        ESP_LOGE(TAG, "dhcp_start 不在 %s 网段内", cfg->usb_netmask);
        return ESP_ERR_INVALID_ARG;
    }
    if (start_h <= ip_h && ip_h <= start_h + size - 1) {
        ESP_LOGE(TAG, "地址池把 P4 自己 (%s) 也圈进去了 —— lwIP 会拒绝", cfg->usb_ip);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t end_h = start_h + size - 1;
    if (end_h >= bcast_h) {
        ESP_LOGE(TAG, "地址池末端越出网段（start=%s size=%" PRIu32 "）", cfg->dhcp_start, size);
        return ESP_ERR_INVALID_ARG;
    }

    usb_net_ip4_set_host(pool_start, start_h);
    usb_net_ip4_set_host(pool_end, end_h);
    *pool_size = size;
    return ESP_OK;
}

esp_err_t usb_net_start(const usb_net_config_t *cfg)
{
    if (g_usbnet.started) {
        ESP_LOGE(TAG, "已经 start 过了（NCM 是静态单例；先 usb_net_stop()）");
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 编译期模式是唯一权威（plan §0.1 的降级开关） ---- */
    usbnet_mode_t mode = cfg->mode;
#if CONFIG_APP_USBNET_MODE_BRIDGE
    if (mode != USBNET_MODE_BRIDGE) {
        ESP_LOGW(TAG, "编译期是 BRIDGE（CONFIG_APP_USBNET_MODE_BRIDGE=y）→ 忽略 cfg->mode");
        mode = USBNET_MODE_BRIDGE;
    }
#else
    if (mode != USBNET_MODE_NAPT) {
        ESP_LOGW(TAG, "编译期是 NAPT（CONFIG_APP_USBNET_MODE_BRIDGE=n）→ 忽略 cfg->mode");
        mode = USBNET_MODE_NAPT;
    }
#endif

#if !USB_NET_HAVE_TINYUSB_NET
    /* 探测模式（mode a/b）里 CONFIG_TINYUSB_NET_MODE_NCM=n：esp_tinyusb 的 net 类驱动
     * （tinyusb_net.c）根本不参与编译，NCM 描述符会是"幽灵功能"。直接拒绝，别把
     * 半个设备装进复合层。 */
    ESP_LOGE(TAG, "CONFIG_TINYUSB_NET_MODE_NCM=n —— 本构建没有 NCM 类驱动，usb_net 不可用");
    return ESP_ERR_NOT_SUPPORTED;
#endif

#if !CONFIG_LWIP_IPV4_NAPT
    if (mode == USBNET_MODE_NAPT) {
        /* I3 契约：编译期没开 NAPT 时必须返回 ESP_ERR_NOT_SUPPORTED */
        ESP_LOGE(TAG, "CONFIG_LWIP_IPV4_NAPT=n —— NAPT 模式不可用（检查 components/usb_net/Kconfig 的 select）");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    memset(&g_usbnet, 0, sizeof(g_usbnet));
    g_usbnet.mode = mode;

    /* ---- 文本参数落地（调用方可能传栈上临时串） ---- */
    const char *ip_s = cfg->usb_ip ? cfg->usb_ip : USB_NET_DEFAULT_IP;
    const char *mask_s = cfg->usb_netmask ? cfg->usb_netmask : USB_NET_DEFAULT_MASK;
    const char *start_s = cfg->dhcp_start ? cfg->dhcp_start : USB_NET_DEFAULT_START;
    snprintf(g_usbnet.ip_str, sizeof(g_usbnet.ip_str), "%s", ip_s);
    snprintf(g_usbnet.mask_str, sizeof(g_usbnet.mask_str), "%s", mask_s);
    snprintf(g_usbnet.dhcp_start_str, sizeof(g_usbnet.dhcp_start_str), "%s", start_s);
    if (cfg->dns_override) {
        snprintf(g_usbnet.dns_override, sizeof(g_usbnet.dns_override), "%s", cfg->dns_override);
    }

    /* 用落地后的副本做所有解析 */
    usb_net_config_t c = *cfg;
    c.usb_ip = g_usbnet.ip_str;
    c.usb_netmask = g_usbnet.mask_str;
    c.dhcp_start = g_usbnet.dhcp_start_str;

    esp_ip4_addr_t ip4, mask4, pool_start, pool_end;
    uint32_t pool_size = 0;
    esp_err_t err = usb_net_prepare_addressing(&c, &ip4, &mask4, &pool_start, &pool_end, &pool_size);
    if (err != ESP_OK) {
        return err;
    }
    g_usbnet.dhcp_pool = pool_size;
    snprintf(g_usbnet.dhcp_end_str, sizeof(g_usbnet.dhcp_end_str), IPSTR, IP2STR(&pool_end));

    /* ---- NCM 段大小 / L3 MTU ---- */
    g_usbnet.seg_size = (c.mtu > 0) ? c.mtu : USB_NET_NCM_SEG_SIZE;
    if (g_usbnet.seg_size != USB_NET_NCM_SEG_SIZE) {
        ESP_LOGW(TAG, "mtu=%u 与 tinyusb 的 CFG_TUD_NET_MTU=%u 不一致：NCM NTB 缓冲是按后者编译的，"
                      "端到端可跑但请确认主机侧也按 %u 协商",
                 (unsigned)g_usbnet.seg_size, USB_NET_NCM_SEG_SIZE, (unsigned)g_usbnet.seg_size);
    }

    /* ---- MAC ---- */
    usb_net_resolve_mac(&c);
    usb_net_mac_to_str(g_usbnet.mac, g_usbnet.mac_str);
    ESP_LOGI(TAG, "USB 网卡 MAC = %s（NCM iMACAddress 字符串同值）", g_usbnet.mac_str);

#if USB_NET_HAVE_TINYUSB_NET
    g_usbnet.tx_lock = xSemaphoreCreateMutex();
    if (!g_usbnet.tx_lock) {
        ESP_LOGE(TAG, "tx_lock 创建失败");
        return ESP_ERR_NO_MEM;
    }
#endif

    /* ---- 注册进 I1 复合层（必须在 usb_composite_prepare() 之前） ---- */
    err = usb_net_ncm_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NCM 注册失败: %s（usb_composite_prepare() 已经跑过了？register 必须在前）",
                 esp_err_to_name(err));
        goto fail;
    }

    /* ---- esp_netif：USB 网卡 + DHCP server（AUTOUP） ---- */
    memset(&s_ip_info, 0, sizeof(s_ip_info));
#if CONFIG_APP_USBNET_MODE_BRIDGE
    /* 桥接模式：USB netif 是**透明二层端口**，不能给它 IP（PC 的地址来自上游 AP）。
     * 这里刻意留 0.0.0.0，lwIP 就不会在这个接口上参与任何 L3 行为。 */
    ESP_LOGW(TAG, "桥接模式：USB netif 不配 IP（%s/%s 只用于日志），PC 从上游 AP 取地址",
             g_usbnet.ip_str, g_usbnet.mask_str);
#else
    s_ip_info.ip = ip4;
    s_ip_info.netmask = mask4;
    s_ip_info.gw = ip4;   /* ⚠️ DHCP server 把 netif 的 gw 当成 option 3（dhcpserver.c:466-476），必须是 P4 自己 */
#endif

    memset(&s_base_cfg, 0, sizeof(s_base_cfg));
    /* ⚠️ flags 里**没有** ESP_NETIF_FLAG_IPV6_AUTOCONFIG_ENABLED —— 这就是"USB netif 上关 IPv6"的实现，
     *    见 usb_net_assert_ipv6_off() 的完整推导。 */
    s_base_cfg.flags = ESP_NETIF_FLAG_AUTOUP;
#if !CONFIG_APP_USBNET_MODE_BRIDGE
    s_base_cfg.flags |= ESP_NETIF_DHCP_SERVER;
#endif
    s_base_cfg.ip_info = &s_ip_info;
    s_base_cfg.if_key = USB_NET_IF_KEY;
    s_base_cfg.if_desc = USB_NET_IF_DESC;
    s_base_cfg.route_prio = 10;             /* 低优先级：绝不抢 STA（WIFI_STA_DEF 是 100）的默认路由 */
    s_base_cfg.mtu = (uint16_t)((g_usbnet.seg_size > 14) ? (g_usbnet.seg_size - 14) : 0);

    memset(&s_driver_cfg, 0, sizeof(s_driver_cfg));
    s_driver_cfg.handle = (void *)&g_usbnet;    /* 必须非 NULL（esp_netif_lwip.c:768） */
    s_driver_cfg.transmit = usb_net_netif_transmit;
    s_driver_cfg.driver_free_rx_buffer = usb_net_netif_free_rx;

    memset(&s_lwip_cfg, 0, sizeof(s_lwip_cfg));
    s_lwip_cfg.lwip.init_fn = ethernetif_init;
    s_lwip_cfg.lwip.input_fn = ethernetif_input;

    esp_netif_config_t ncfg = {
        .base = &s_base_cfg,
        .driver = &s_driver_cfg,
        .stack = &s_lwip_cfg,
    };
    g_usbnet.netif = esp_netif_new(&ncfg);
    if (!g_usbnet.netif) {
        ESP_LOGE(TAG, "esp_netif_new 失败（if_key=%s 已存在？）", USB_NET_IF_KEY);
        err = ESP_FAIL;
        goto fail;
    }
    err = esp_netif_set_mac(g_usbnet.netif, g_usbnet.mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_mac: %s", esp_err_to_name(err));
        goto fail_netif;
    }

#if !CONFIG_APP_USBNET_MODE_BRIDGE
    {
        /* DHCP server 的地址池 / 租约 / option 6 —— **必须在 start 之前**设：
         * esp_netif_lwip.c:2631-2633 在 server 已 STARTED 时拒绝任何 ESP_NETIF_OP_SET。 */
        dhcps_lease_t lease;
        memset(&lease, 0, sizeof(lease));
        lease.enable = true;
        /* ⚠️ dhcps_lease_t 用的是 **lwIP 的** ip4_addr_t（dhcpserver.h:38-42），
         *    与 esp_ip4_addr_t 是**不同的 struct 类型**（虽然都只有一个 u32）→ 不能整体赋值，
         *    必须逐字段拷 .addr（verifier 实测：incompatible types when assigning）。 */
        lease.start_ip.addr = pool_start.addr;
        lease.end_ip.addr = pool_end.addr;
        err = esp_netif_dhcps_option(g_usbnet.netif, ESP_NETIF_OP_SET,
                                     ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "dhcps_option(地址池 %s~%s): %s", g_usbnet.dhcp_start_str,
                     g_usbnet.dhcp_end_str, esp_err_to_name(err));
            goto fail_netif;
        }

        uint32_t lease_s = (uint32_t)CONFIG_APP_USBNET_DHCP_LEASE_S;
        esp_netif_dhcps_option(g_usbnet.netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                               &lease_s, sizeof(lease_s));

        uint8_t offer_dns = OFFER_DNS;
        esp_netif_dhcps_option(g_usbnet.netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer_dns, sizeof(offer_dns));
    }
#endif

    /* ---- 起来（AUTOUP 会把 lwIP netif 置 up，DHCP server 随之启动） ---- */
    esp_netif_action_start(g_usbnet.netif, 0, 0, 0);
    if (!esp_netif_is_netif_up(g_usbnet.netif)) {
        ESP_LOGE(TAG, "USB netif 没起来");
        err = ESP_FAIL;
        goto fail_netif;
    }

    /* ---- 事件：上游 DNS 刷新 + DHCP 客户端统计 ---- */
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, usb_net_event, NULL,
                                             (esp_event_handler_instance_t *)&g_usbnet.h_ip_event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IP_EVENT_STA_GOT_IP handler: %s（DNS 只能靠 usb_net_set_upstream 刷一次）",
                 esp_err_to_name(err));
        g_usbnet.h_ip_event = NULL;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, usb_net_event,
                                             NULL, (esp_event_handler_instance_t *)&g_usbnet.h_ip_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ASSIGNED_IP_TO_CLIENT handler: %s（clients 统计会恒为 0）", esp_err_to_name(err));
        g_usbnet.h_ip_client = NULL;
    }

    /* ---- esp_tinyusb 的 net 类驱动（**必须在 tinyusb_driver_install 之前**，见 I3 契约） ---- */
#if USB_NET_HAVE_TINYUSB_NET
    {
        tinyusb_net_config_t nc;
        memset(&nc, 0, sizeof(nc));
        memcpy(nc.mac_addr, g_usbnet.mac, 6);
        nc.on_recv_callback = usb_net_usb_rx_cb;
        nc.free_tx_buffer = usb_net_tx_free_cb;
        nc.user_context = NULL;
        err = tinyusb_net_init(&nc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_net_init: %s", esp_err_to_name(err));
            goto fail_all;
        }
    }
#endif

    /* ---- NAPT（开在**内侧** = USB netif） ---- */
#if CONFIG_APP_USBNET_MODE_BRIDGE
    ESP_LOGW(TAG, "⚠️ 编译期桥接模式（CONFIG_APP_USBNET_MODE_BRIDGE=y）：这是**降级选项**，"
                  "上游 AP 会看到 2 个客户端；Wi-Fi STA 在 3 地址模式下无法被透明二层桥接"
                  "（IDF examples/network/bridge/README.md:164），本路径**未在真机验证过**");
    err = usb_net_bridge_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge 初始化失败: %s", esp_err_to_name(err));
        goto fail_all;
    }
#else
    err = esp_netif_napt_enable(g_usbnet.netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_napt_enable(USB netif): %s"
                      "（返回 ESP_ERR_NOT_SUPPORTED = CONFIG_LWIP_IPV4_NAPT 没开）", esp_err_to_name(err));
        goto fail_all;
    }
    g_usbnet.napt_enabled = true;
    ESP_LOGI(TAG, "NAPT 已开在**内侧** USB netif（lwIP ip_napt_forward 只看 inp->napt）");
#endif

    g_usbnet.started = true;
    ESP_LOGI(TAG, "USB 网卡就绪: %s/%s  池 %s~%s (%u 个)  租约 %d s  gw=%s  模式=%s",
             g_usbnet.ip_str, g_usbnet.mask_str, g_usbnet.dhcp_start_str, g_usbnet.dhcp_end_str,
             (unsigned)g_usbnet.dhcp_pool, CONFIG_APP_USBNET_DHCP_LEASE_S, g_usbnet.ip_str,
             (mode == USBNET_MODE_NAPT) ? "NAPT" : "BRIDGE");

    usb_net_assert_ipv6_off();

    /* ---- 上游（可后补） ---- */
    if (cfg->upstream_netif) {
        err = usb_net_set_upstream(cfg->upstream_netif);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "usb_net_set_upstream: %s（可以稍后用 usb_net_set_upstream() 补）",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "upstream_netif 未给 → 还不能转发；请 net_hosted_start() 后调 usb_net_set_upstream()");
    }

    ESP_LOGI(TAG, "⚠️ 未验证（设备未连接）：Windows 认不认 NCM、NAPT 会话、DNS 透传、DHCP 租约、C6 固件版本");
    return ESP_OK;

fail_all:
    if (g_usbnet.h_ip_event) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_usbnet.h_ip_event);
    }
    if (g_usbnet.h_ip_client) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, g_usbnet.h_ip_client);
    }
#if USB_NET_HAVE_TINYUSB_NET
    tinyusb_net_deinit();
#endif
fail_netif:
    if (g_usbnet.netif) {
        esp_netif_destroy(g_usbnet.netif);
        g_usbnet.netif = NULL;
    }
fail:
#if USB_NET_HAVE_TINYUSB_NET
    if (g_usbnet.tx_lock) {
        vSemaphoreDelete((SemaphoreHandle_t)g_usbnet.tx_lock);
        g_usbnet.tx_lock = NULL;
    }
#endif
    g_usbnet.started = false;
    return err;
}

esp_err_t usb_net_stop(void)
{
    if (!g_usbnet.started) {
        return ESP_OK;
    }

    if (g_usbnet.h_ip_event) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_usbnet.h_ip_event);
        g_usbnet.h_ip_event = NULL;
    }
    if (g_usbnet.h_ip_client) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, g_usbnet.h_ip_client);
        g_usbnet.h_ip_client = NULL;
    }

#if CONFIG_APP_USBNET_MODE_BRIDGE
    usb_net_bridge_stop();
#else
    if (g_usbnet.napt_enabled && g_usbnet.netif) {
        esp_netif_napt_disable(g_usbnet.netif);
        g_usbnet.napt_enabled = false;
    }
    if (g_usbnet.netif) {
        esp_netif_dhcps_stop(g_usbnet.netif);
    }
#endif

#if USB_NET_HAVE_TINYUSB_NET
    tinyusb_net_deinit();
#endif

    if (g_usbnet.netif) {
        esp_netif_action_stop(g_usbnet.netif, 0, 0, 0);
        esp_netif_destroy(g_usbnet.netif);
        g_usbnet.netif = NULL;
    }

#if USB_NET_HAVE_TINYUSB_NET
    if (g_usbnet.tx_lock) {
        vSemaphoreDelete((SemaphoreHandle_t)g_usbnet.tx_lock);
        g_usbnet.tx_lock = NULL;
    }
#endif

    g_usbnet.started = false;
    g_usbnet.upstream = NULL;
    ESP_LOGW(TAG, "USB 网卡已停。⚠️ 注意：I1 里的 NCM 功能注册已经生效，"
                  "重新 start 需要重新 usb_composite_prepare/start（不是运行期可热插拔的）");
    return ESP_OK;
}

esp_err_t usb_net_set_upstream(esp_netif_t *upstream_netif)
{
    if (!upstream_netif) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_usbnet.started) {
        return ESP_ERR_INVALID_STATE;
    }

    g_usbnet.upstream = upstream_netif;

    if (g_usbnet.mode == USBNET_MODE_BRIDGE) {
        /* ⚠️ 必须用**同一个宏**守卫（Lead 2026-10-21 修复，链接期实测）：
         *   `usb_net_bridge.c` 只在 CONFIG_APP_USBNET_MODE_BRIDGE=y 时才进 SRCS，
         *   而这里原先只有运行期 `if` ⇒ 符号被无条件引用 ⇒ NAPT（生产）模式链接失败：
         *     undefined reference to `usb_net_bridge_set_upstream'
         *   本文件其余 4 处 bridge 调用都有 `#if` 守卫，只有这一处漏了。
         *   教训与 §7 同族：**"CMake 有条件编译某文件" 必须与 "C 里条件引用它" 用同一个宏**，
         *   否则只有链接期才暴露。 */
#if CONFIG_APP_USBNET_MODE_BRIDGE
        return usb_net_bridge_set_upstream(upstream_netif);
#else
        ESP_LOGE(TAG, "请求桥接模式，但 usb_net_bridge.c 未编译"
                      "（CONFIG_APP_USBNET_MODE_BRIDGE=n）");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    /* 网关：默认路由必须走上游（转发时 lwIP 靠它选 outp；softap_sta 示例同样这么设） */
    esp_netif_set_default_netif(upstream_netif);
    g_usbnet.upstream_is_default = true;

    /* 上游此刻可能已经有 IP 也可能还没有：两种都刷一次，没 IP 时函数会自己等 IP_EVENT。
     * 契约里"运行中补上/替换上游 netif"= 这里是幂等的。 */
    usb_net_apply_dns_from_upstream();

    ESP_LOGI(TAG, "上游 netif = %p (%s)；DNS 透传/默认路由已按它刷新",
             (void *)upstream_netif, esp_netif_get_ifkey(upstream_netif));
    return ESP_OK;
}
