// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I3 契约 —— **网络桥接**（USB 网卡 ↔ esp_netif）
//
//   生产者（本文件）：Lead            实现：components/usb_net/（WS-net）
//   消费者：main/main.cpp（启动顺序）· components/net_hosted/（上游 STA 联通）
//
// 架构定案（plan §0.1）：**NAPT 路由，不做 L2 桥接**。
//   PC 落在 P4 后面的私网（默认 192.168.77.0/24，P4 = .1），P4 跑 DHCP server，
//   用 lwIP NAPT 把 PC 的流量 NAT 出去、经 Wi-Fi STA（C6 射频）上网。
//   ⚠️ 降级开关 `CONFIG_APP_USBNET_MODE_BRIDGE` 保留 L2 桥接作为编译期备选。
//
// 为什么不用 L2 桥接（plan §0.1 三条决定性理由）：
//   ① 上游 AP 只看到 1 个客户端 → 酒店/企业/访客 Wi-Fi 也能用；
//   ② 避免 PC 出现"同网段双路由"这类极难排查的间歇故障；
//   ③ Espressif 在 hosted + NAPT 同一套栈里已有官方先例。
// ⚠️ NAPT 只管 IPv4：USB netif 上必须**关掉 IPv6（RA/DHCPv6）**，
//    否则 PC 拿到 IPv6 地址却没有回程路由 → 表现为"部分站点卡住"（plan §0.1）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 桥接模式（编译期二选一，见 `CONFIG_APP_USBNET_MODE_*`） */
typedef enum {
    USBNET_MODE_NAPT = 0,   /**< 默认：P4 做网关 + DHCP server + NAPT */
    USBNET_MODE_BRIDGE = 1, /**< 降级：P4 做透明二层桥，PC 直接从上游 AP 取 IP */
} usbnet_mode_t;

typedef struct {
    usbnet_mode_t mode;

    /** USB 网卡 MAC。全 0 = 用 STA 的 MAC（plan §5.4 建议，避免与上游撞 MAC） */
    uint8_t mac[6];

    /** NAPT 模式：P4 在 USB 侧的静态地址（`a.b.c.d` 点分字符串） */
    const char *usb_ip;      /**< 默认 "192.168.77.1" */
    const char *usb_netmask; /**< 默认 "255.255.255.0" */
    /** DHCP server 起始地址（默认 192.168.77.2）；分配池大小 64 */
    const char *dhcp_start;
    /** DHCP 下发用的 DNS：NULL = 透传上游 STA 拿到的 DNS（plan §0.1：**不做 DNS 代理**） */
    const char *dns_override;

    /** 上游 netif 句柄（由 net_hosted 提供）。桥接模式下必需；NAPT 模式下用于取 DNS */
    esp_netif_t *upstream_netif;

    /** NCM/TinyUSB 侧最大段大小（MTU 相关），0 = 默认 */
    uint16_t mtu;
} usb_net_config_t;

/** 默认配置：NAPT / MAC 随 STA / 192.168.77.1/24 / DNS 透传 */
void usb_net_config_default(usb_net_config_t *out);

/* ============================ 生命周期 ============================ */

/**
 * @brief 建 USB 网卡：`tinyusb_net_init()` + `esp_netif`（DHCP server）+ NAPT 配置。
 *
 * ⚠️ **必须在 `usb_composite_start()` 之前调用**：NCM 走 esp_tinyusb 的 net 类驱动，
 *    它需要在 TinyUSB install 之前拿到自己的配置（与 MTP 同理，见 probe-contract §3.2）。
 *
 * 本函数**不**负责连 Wi-Fi；上游由 `components/net_hosted/` 负责，
 * 这里只要求 `cfg->upstream_netif` 在**真正开始转发之前**有效（可用 `usb_net_set_upstream()` 补）。
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（已启动）；ESP_ERR_INVALID_ARG；
 *         ESP_ERR_NOT_SUPPORTED（编译期没开 NAPT：`CONFIG_LWIP_IPV4_NAPT=n`）
 */
esp_err_t usb_net_start(const usb_net_config_t *cfg);

/** 停掉 USB 网卡（释放 esp_netif/DHCP server/NAT），保留 TinyUSB 其它功能 */
esp_err_t usb_net_stop(void);

/**
 * @brief 运行中补上/替换上游 netif（STA 起来得比 USB 晚时的常用姿势）。
 *
 * NAPT 模式下会刷新 NAT 表；桥接模式下会重建桥。
 */
esp_err_t usb_net_set_upstream(esp_netif_t *upstream_netif);

/* ============================ 查询 ============================ */

/** USB 网卡的 esp_netif 句柄（未启动返回 NULL） */
esp_netif_t *usb_net_get_netif(void);

/** 已有几个 DHCP 客户端（= 有几台 PC 在用） */
uint32_t usb_net_client_count(void);

typedef struct {
    bool     up;             /**< USB 网卡已就绪 */
    bool     upstream_up;    /**< 上游有 IP */
    uint32_t rx_packets;     /**< USB → lwIP */
    uint32_t tx_packets;     /**< lwIP → USB */
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t nat_entries;    /**< NAPT 会话数 */
    uint32_t clients;        /**< DHCP 租约数 */
} usb_net_stats_t;

void usb_net_get_stats(usb_net_stats_t *out);

/* ============================ 适配层回调（由 usb_net 实现，供 esp_netif 调用） ============================ */

/**
 * @brief 从 lwIP 侧发一帧到 USB（`esp_netif` 的 `transmit` 回调最终落到这里）。
 *
 * 声明在这里是为了让 verifier 能独立检查"这条通路有没有绕过 usb_net"。
 * 普通调用者**不要**直接用它。
 */
esp_err_t usb_net_lwip_to_usb(void *buffer, uint16_t len);

#ifdef __cplusplus
}
#endif
