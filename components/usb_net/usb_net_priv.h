// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I3 内部（只给 components/usb_net/*.c 用，不进 public include 路径）
//
// 外部可见契约是 include/usb_net.h（**冻结，Lead 所有**）。本文件只放实现内部的共享状态。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "sdkconfig.h"

#include "usb_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 冻结值（docs/contracts/frozen-allocation.md §1/§2/§6）
 * NCM：IF3 控制 + IF4 数据（带 IAD）、EP5 int IN 16/interval 8、EP6 bulk IN+OUT 512(HS)/64(FS)
 *
 * ⚠️ 这里的宏**同时**喂给「I1 注册的端点需求」与「描述符片段」两处，唯一来源，防止两边漂移
 *    （复合层的 selfcheck 会逐项比对描述符与注册需求）。
 * ⚠️ 这里**没有任何硬编码的接口号/端点号** —— 号由 I1 复合层分配，见 usb_net_ncm.c 的 alloc 校验。
 * -------------------------------------------------------------------------- */
#define USB_NET_IF_KEY              "USB_NCM_DEF"
#define USB_NET_IF_DESC             "MeidaP4 USB NCM (NAPT gateway)"
#define USB_NET_NCM_SEG_SIZE        1514        /* wMaxSegmentSize（= tinyusb CFG_TUD_NET_MTU） */
#define USB_NET_NCM_NOTIF_MPS       16          /* 通知端点：ncm_notify_t 正好 16 B */
#define USB_NET_NCM_NOTIF_INTERVAL  8           /* ms */
#define USB_NET_NCM_BULK_MPS_HS     512
#define USB_NET_NCM_BULK_MPS_FS     64

#define USB_NET_MAX_CLIENTS         8           /* 只用于统计"几台 PC 在用" */
#define USB_NET_MAC_STR_LEN         13          /* "AABBCCDDEEFF" + NUL */

typedef struct {
    uint8_t        mac[6];
    esp_ip4_addr_t ip;
} usb_net_client_t;

/** usb_net 的全部运行时状态（单例；NCM 是静态单例设备，见 sta2eth 的注释） */
typedef struct {
    bool           started;
    usbnet_mode_t  mode;

    esp_netif_t   *netif;         /**< USB 侧 netif（NAPT 网关/桥接端口） */
    esp_netif_t   *upstream;      /**< 上游 STA netif */
    bool           upstream_is_default;

    uint8_t        mac[6];
    char           mac_str[USB_NET_MAC_STR_LEN];   /**< NCM iMACAddress 用（12 个大写十六进制） */
    uint16_t       seg_size;                       /**< NCM wMaxSegmentSize */

    char           ip_str[16];
    char           mask_str[16];
    char           dhcp_start_str[16];
    char           dhcp_end_str[16];
    char           dns_str[16];                    /**< 已下发/将下发的 DNS（"" = 还没有） */
    char           dns_override[16];               /**< "" = 透传上游 */
    uint32_t       dhcp_pool;                      /**< 地址池大小 */

    bool           napt_enabled;

    usb_net_client_t clients[USB_NET_MAX_CLIENTS];
    uint8_t        client_count;                   /**< 表里不同 MAC 数（累计，无租约回收事件） */

    /* 事件处理实例（stop 时注销） */
    void          *h_ip_event;
    void          *h_ip_client;

    /* 统计 */
    uint32_t       rx_packets, rx_bytes;           /**< USB → lwIP */
    uint32_t       tx_packets, tx_bytes;           /**< lwIP → USB */
    uint32_t       rx_dropped, tx_dropped;

#if CONFIG_TINYUSB_NET_MODE_NCM || CONFIG_TINYUSB_NET_MODE_ECM_RNDIS
    void          *tx_lock;                        /**< SemaphoreHandle_t：esp_tinyusb 的 send_sync 不可重入 */
#endif
} usb_net_ctx_t;

extern usb_net_ctx_t g_usbnet;

/* ===================== usb_net_ncm.c ===================== */

/** 把 NCM 功能注册进 I1 复合层（必须在 usb_composite_prepare() 之前、且**最后**注册以拿到 IF3/IF4） */
esp_err_t usb_net_ncm_register(void);

/** NCM 的 iMACAddress 字符串（12 个大写十六进制） */
const char *usb_net_ncm_mac_str(void);

/* ===================== usb_net_bridge.c（仅 CONFIG_APP_USBNET_MODE_BRIDGE 编译） ===================== */

/** 桥接：把 USB 侧收到的一帧（802.3）直接送进 STA 射频。返回 ESP_OK 表示已接管该 buffer 的生命周期 */
esp_err_t usb_net_bridge_usb_to_wifi(const void *frame, uint16_t len);

/** 桥接：初始化（此时上游可能还没起来） */
esp_err_t usb_net_bridge_init(void);

/** 桥接：挂上/替换上游（注册 esp_wifi_internal_reg_rxcb） */
esp_err_t usb_net_bridge_set_upstream(esp_netif_t *upstream);

/** 桥接：撤销 rxcb */
esp_err_t usb_net_bridge_stop(void);

/** 桥接：释放一帧从射频侧来的、由 esp_wifi 拥有的 buffer */
void usb_net_bridge_free_rx_buffer(void *buffer);

/* ===================== usb_net.c 内部小工具 ===================== */

/** 主机序 IPv4 <-> esp_ip4_addr_t（esp_ip4_addr_t.addr 是**网络字节序**，用自反的 esp_netif_htonl 转换） */
uint32_t usb_net_ip4_host(const esp_ip4_addr_t *a);
void     usb_net_ip4_set_host(esp_ip4_addr_t *a, uint32_t h);

/** 上游 DNS 变化时刷新 DHCP option 6（不做代理，直接透传） */
void usb_net_apply_dns_from_upstream(void);

#ifdef __cplusplus
}
#endif
