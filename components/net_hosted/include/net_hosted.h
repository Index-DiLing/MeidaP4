// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · WS-net 上游侧 —— esp_hosted(3.0.7) + esp_wifi_remote 的 Wi-Fi STA 生命周期
//
// 角色（plan §0.1）：P4 自己没有射频，上游经 **C6** 出去。本组件只负责把 STA 拉起来、
// 维护断线重连、并把上游 `esp_netif_t *` 交给 USB 侧（I3：components/usb_net）。
// NCM / DHCP server / NAPT **不在这里**，在 components/usb_net。
//
// ⚠️ 不做的事：
//   * 不重复初始化 esp_hosted —— prod 配置是 `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=y`，
//     esp_hosted 在 `app_main` 之前已由构造函数建好传输并握手（`eh_host_auto_init_ctor`）。
//     esp_hosted 还提供了 `--wrap=esp_netif_init` / `--wrap=esp_event_loop_create_default`
//     （`host/eh_host_core/src/eh_host_idf_idempotent_wraps.c`），所以下面重复调用它们是幂等的。
//   * 不碰 USB、不碰 NCM。
//
// 线程/上下文：事件回调在默认事件循环任务里跑（**不得阻塞**）；重连用一次性 esp_timer，
// 不在事件回调里 vTaskDelay（那会卡死整个默认事件循环）。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 上游 STA 的状态机（也是给网页/遥测看的） */
typedef enum {
    NET_HOSTED_STATE_IDLE = 0,   /**< 未启动 */
    NET_HOSTED_STATE_CP_WAIT,    /**< 已启动，等 C6 握手/esp_wifi_init */
    NET_HOSTED_STATE_SCAN_ONLY,  /**< SSID 未配置（或为空）→ 只扫描不连接 */
    NET_HOSTED_STATE_CONNECTING, /**< 已 esp_wifi_connect()，等 STA_CONNECTED */
    NET_HOSTED_STATE_CONNECTED,  /**< L2 已连上，等 DHCP 拿到 IP */
    NET_HOSTED_STATE_GOT_IP,     /**< 有 IP —— 可以转发（usb_net 用这个判断 upstream_up） */
    NET_HOSTED_STATE_RETRYING,   /**< 掉线，等重连定时器 */
    NET_HOSTED_STATE_FAILED,     /**< 重试超上限（CONFIG_NET_HOSTED_CONNECT_RETRY_MAX） */
} net_hosted_state_t;

typedef struct {
    net_hosted_state_t state;
    bool     connected;              /**< WIFI_EVENT_STA_CONNECTED 之后为 true */
    bool     has_ip;                 /**< IP_EVENT_STA_GOT_IP 之后为 true */
    uint32_t connect_attempts;       /**< 累计 esp_wifi_connect() 次数 */
    uint32_t disconnects;            /**< 累计断开次数 */
    uint32_t last_disconnect_reason; /**< wifi_err_reason_t（最近一次） */
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
    esp_ip4_addr_t dns;              /**< 主 DNS（由 DHCP 客户端写入；usb_net 透传用） */

    /* ---- C6 握手版本号（开机打印一次，留档用） ---- */
    bool     cp_ver_valid;
    uint32_t cp_chip_id;             /**< CP chip id（RPC 取回） */
    char     cp_target[16];          /**< CP 的 idf_target，如 "esp32c6" */
    char     cp_fw_version[24];      /**< CP 固件版本，如 "3.0.7" */
    int32_t  cp_fw_revision;
} net_hosted_status_t;

/**
 * @brief 拉起上游 STA（幂等：第二次调用直接返回 ESP_OK）。
 *
 * 顺序：nvs_flash_init → esp_netif_init → esp_event_loop_create_default →
 *       esp_netif_create_default_wifi_sta → 注册事件 → esp_wifi_init →
 *       （有 SSID）set_config + esp_wifi_start + connect /（无 SSID）扫描一次后停在这里。
 *
 * @return ESP_OK；ESP_FAIL（netif 创建失败）；esp_wifi_init/start 的原样错误
 *         （例如 `CONFIG_ESP_HOSTED_HOST_FEAT_WIFI` 缺失时是 ESP_ERR_NOT_SUPPORTED）
 */
esp_err_t net_hosted_start(void);

/** 停掉 STA（幂等）。用于降级/测试，正常流程不需要 */
esp_err_t net_hosted_stop(void);

/**
 * @brief 上游 `esp_netif_t *`（= `WIFI_STA_DEF`）。未启动返回 NULL。
 *
 * 给 `usb_net_config_t.upstream_netif` 用；也可 `usb_net_set_upstream()` 后补。
 */
esp_netif_t *net_hosted_upstream_netif(void);

/** 上游是否已拿到 IPv4（= 可以真的转发出去） */
bool net_hosted_has_ip(void);

/** 上游 L2 是否已关联（可能还没拿到 IP） */
bool net_hosted_is_connected(void);

/**
 * @brief 取 STA 的 MAC（给 NCM 网卡当 MAC 用；不阻塞、不发 RPC）。
 *
 * 实现是读 `esp_wifi_get_mac(WIFI_IF_STA)`（esp_hosted 的 glue 里有缓存/ RPC 两条路，
 * 已关联后是本地缓存）。未启动或取不到返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t net_hosted_get_sta_mac(uint8_t mac[6]);

/** 取状态快照（可传 NULL） */
void net_hosted_get_status(net_hosted_status_t *out);

/** 状态枚举 → 人类可读字符串（日志/遥测用） */
const char *net_hosted_state_str(net_hosted_state_t st);

/** 最近一次扫描到的 AP 数量（scan-only 模式留档用）；未扫描返回 0 */
uint32_t net_hosted_last_scan_count(void);

#ifdef __cplusplus
}
#endif
