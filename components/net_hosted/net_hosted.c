// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · WS-net 上游侧实现（esp_hosted STA）
//
// 见 include/net_hosted.h 的说明。本文件只做 STA 生命周期，不做 NCM/NAPT。

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "net_hosted.h"

#if CONFIG_ESP_HOSTED
#include "esp_hosted.h"   /* esp_hosted_get_coprocessor_fwversion / _get_cp_info（compat 公开面） */
#define NET_HOSTED_CP_API 1
#else
#define NET_HOSTED_CP_API 0
#endif

static const char *TAG = "net_hosted";

/* CP 固件版本来自 PRIV 握手的 RPC；esp_hosted.host 侧当前版本（dependencies.lock 锁死） */
#define NET_HOSTED_HOST_LIB_VER "3.0.7"

/* ⚠️ Kconfig 的 bool 在 =n 时**没有**宏（sdkconfig.h 里是 #undef），所以任何把 CONFIG_xxx
 *    当**C 表达式**用的地方都必须先#if收敛成宏，否则会 "CONFIG_xxx undeclared"（verifier 实测踩过）。 */
#if CONFIG_NET_HOSTED_WPA3_SAE
#define NET_HOSTED_AUTH_STR "WPA3-SAE"
#else
#define NET_HOSTED_AUTH_STR "WPA2-PSK(及以上)"
#endif

/* 断开后重连延迟（ms） */
#define NET_HOSTED_RECONNECT_MS ((uint32_t)CONFIG_NET_HOSTED_RECONNECT_MS)
/* 连续重连上限，0 = 无限 */
#define NET_HOSTED_RETRY_MAX    ((uint32_t)CONFIG_NET_HOSTED_CONNECT_RETRY_MAX)

typedef struct {
    bool               started;
    bool               ssid_configured;
    bool               cp_logged;
    esp_netif_t       *sta;
    esp_timer_handle_t retry_timer;
    net_hosted_status_t st;
    uint32_t           scan_count;
} net_hosted_ctx_t;

static net_hosted_ctx_t s;

const char *net_hosted_state_str(net_hosted_state_t st)
{
    switch (st) {
    case NET_HOSTED_STATE_IDLE:       return "IDLE";
    case NET_HOSTED_STATE_CP_WAIT:    return "CP_WAIT";
    case NET_HOSTED_STATE_SCAN_ONLY:  return "SCAN_ONLY";
    case NET_HOSTED_STATE_CONNECTING: return "CONNECTING";
    case NET_HOSTED_STATE_CONNECTED:  return "CONNECTED";
    case NET_HOSTED_STATE_GOT_IP:     return "GOT_IP";
    case NET_HOSTED_STATE_RETRYING:   return "RETRYING";
    case NET_HOSTED_STATE_FAILED:     return "FAILED";
    default:                          return "?";
    }
}

/* =====================================================================
 * C6 握手版本号（开机一次，留档用）
 * ===================================================================== */

static void net_hosted_log_cp_version(void)
{
    if (s.cp_logged) {
        return;
    }
    s.cp_logged = true;

#if NET_HOSTED_CP_API
    /* ⚠️ 不在这里 eh_host_init/connect：prod 是 CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=y，
     *    esp_hosted 的构造函数已经把传输建好并握手完成。这两个 API 内部还会再等一次 auto-init
     *    （eh_host_feat_rpc.c:35-42），所以早调是安全的，失败也不 abort（无凭据/无 C6 时还要能启动）。 */
    esp_hosted_coprocessor_fwver_t fw;
    memset(&fw, 0, sizeof(fw));
    int rc = esp_hosted_get_coprocessor_fwversion(&fw);
    if (rc == 0) {
        s.st.cp_ver_valid = true;
        s.st.cp_fw_revision = fw.revision;
        snprintf(s.st.cp_fw_version, sizeof(s.st.cp_fw_version), "%u.%u.%u",
                 (unsigned)fw.major1, (unsigned)fw.minor1, (unsigned)fw.patch1);
        ESP_LOGI(TAG, "C6 握手版本（留档）: CP 固件=%s revision=%d | host esp_hosted=%s",
                 s.st.cp_fw_version, (int)fw.revision, NET_HOSTED_HOST_LIB_VER);
    } else {
        ESP_LOGW(TAG, "C6 握手版本读取失败 (rc=%d) —— esp_hosted 传输没起来？"
                      " host 侧 esp_hosted=%s", rc, NET_HOSTED_HOST_LIB_VER);
    }

    uint32_t chip_id = 0;
    char target[sizeof(s.st.cp_target)] = { 0 };
    if (esp_hosted_get_cp_info(&chip_id, target, sizeof(target)) == 0) {
        s.st.cp_chip_id = chip_id;
        strncpy(s.st.cp_target, target, sizeof(s.st.cp_target) - 1);
        ESP_LOGI(TAG, "C6 握手目标（留档）: chip_id=0x%08x idf_target=\"%s\"",
                 (unsigned)chip_id, s.st.cp_target);
    }
#else
    ESP_LOGW(TAG, "CONFIG_ESP_HOSTED=n —— 没有 C6 射频，本组件只会空转");
#endif
}

/* =====================================================================
 * DNS（透传用；usb_net 只读 status）
 * ===================================================================== */

static void net_hosted_refresh_dns(void)
{
    if (!s.sta) {
        return;
    }
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    if (esp_netif_get_dns_info(s.sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        s.st.dns = dns.ip.u_addr.ip4;
    }
}

/* =====================================================================
 * 断线重连（一次性 esp_timer；**不在事件回调里 vTaskDelay**）
 * ===================================================================== */

static void net_hosted_retry_cb(void *arg)
{
    (void)arg;
    if (!s.started || s.st.connected) {
        return;
    }
    s.st.connect_attempts++;
    s.st.state = NET_HOSTED_STATE_CONNECTING;
    ESP_LOGI(TAG, "重连 #%" PRIu32 " (SSID=\"%s\")", s.st.connect_attempts, CONFIG_NET_HOSTED_WIFI_SSID);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void net_hosted_schedule_retry(void)
{
    /* 用变量而不是宏直接比较：`x >= 0u` 这种常量比较会被 -Werror=type-limits 拦下
     * （verifier 实测踩过 net_hosted.c:158）。 */
    const uint32_t max_retry = NET_HOSTED_RETRY_MAX;
    if (max_retry != 0 && s.st.connect_attempts >= max_retry) {
        s.st.state = NET_HOSTED_STATE_FAILED;
        ESP_LOGE(TAG, "重连次数用尽 (%" PRIu32 "/%" PRIu32 ") → FAILED",
                 s.st.connect_attempts, max_retry);
        return;
    }
    if (!s.retry_timer) {
        return;
    }
    s.st.state = NET_HOSTED_STATE_RETRYING;
    esp_timer_stop(s.retry_timer);   /* 幂等：已在跑就先停掉再重排 */
    esp_err_t err = esp_timer_start_once(s.retry_timer, (uint64_t)NET_HOSTED_RECONNECT_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重连定时器启动失败: %s", esp_err_to_name(err));
    }
}

/* =====================================================================
 * 只看不连：扫描一次并打印（无凭据时也能启动）
 * ===================================================================== */

static void net_hosted_scan_only(void)
{
    ESP_LOGW(TAG, "未配置 SSID（CONFIG_NET_HOSTED_WIFI_SSID 为空）→ 只扫描不连接");
    s.st.state = NET_HOSTED_STATE_SCAN_ONLY;

    wifi_scan_config_t scan = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start: %s（C6 不在/未握手？继续，不 abort）", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_num 失败");
        return;
    }
    s.scan_count = n;
    ESP_LOGI(TAG, "扫描到 %u 个 AP", (unsigned)n);

    uint16_t want = (uint16_t)CONFIG_NET_HOSTED_SCAN_LOG_MAX;
    if (want > n) {
        want = n;
    }
    if (want == 0) {
        return;
    }
    wifi_ap_record_t *recs = calloc(want, sizeof(*recs));
    if (!recs) {
        ESP_LOGW(TAG, "扫描结果缓冲分配失败");
        return;
    }
    uint16_t got = want;
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (uint16_t i = 0; i < got; i++) {
            ESP_LOGI(TAG, "  [%u] \"%s\" rssi=%d ch=%u auth=%u",
                     (unsigned)i, (const char *)recs[i].ssid, (int)recs[i].rssi,
                     (unsigned)recs[i].primary, (unsigned)recs[i].authmode);
        }
    }
    free(recs);
}

/* =====================================================================
 * 事件
 * ===================================================================== */

static void net_hosted_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START（netif 由 esp_hosted 的默认 handler 拉起）");
            if (s.ssid_configured) {
                s.st.state = NET_HOSTED_STATE_CONNECTING;
                s.st.connect_attempts++;
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
                    net_hosted_schedule_retry();
                }
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            s.st.connected = true;
            s.st.state = NET_HOSTED_STATE_CONNECTED;
            s.st.connect_attempts = 0;   /* 连上了，重试计数归零 */
            ESP_LOGI(TAG, "STA 已关联，等 DHCP");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            s.st.connected = false;
            s.st.has_ip = false;
            s.st.disconnects++;
            if (d) {
                s.st.last_disconnect_reason = (uint32_t)d->reason;
            }
            ESP_LOGW(TAG, "STA 断开 (reason=%" PRIu32 ")，%" PRIu32 " ms 后重连（累计断开 %" PRIu32 "）",
                     s.st.last_disconnect_reason, NET_HOSTED_RECONNECT_MS, s.st.disconnects);
            net_hosted_schedule_retry();
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGD(TAG, "WIFI_EVENT_SCAN_DONE");
            break;

        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        if (evt) {
            s.st.ip = evt->ip_info.ip;
            s.st.netmask = evt->ip_info.netmask;
            s.st.gw = evt->ip_info.gw;
        }
        s.st.has_ip = true;
        s.st.connected = true;
        s.st.state = NET_HOSTED_STATE_GOT_IP;
        net_hosted_refresh_dns();
        ESP_LOGI(TAG, "上游就绪: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR " dns=" IPSTR,
                 IP2STR(&s.st.ip), IP2STR(&s.st.netmask), IP2STR(&s.st.gw), IP2STR(&s.st.dns));
        ESP_LOGI(TAG, "⚠️ NAPT 会话与 USB 侧转发由 components/usb_net 负责（本组件不含 NCM）");
    }
}

/* =====================================================================
 * 生命周期
 * ===================================================================== */

esp_err_t net_hosted_start(void)
{
    if (s.started) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除 (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    /* 这两个在 esp_hosted 的 host 侧被 --wrap 成幂等（eh_host_idf_idempotent_wraps.c），
     * 所以即使 auto-init 已经调过也不会 ESP_ERR_INVALID_STATE。 */
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }

    s.st.state = NET_HOSTED_STATE_CP_WAIT;

    /* ⚠️ esp_hosted(hosted 版) 的 esp_netif_create_default_wifi_sta() 不查重名：
     *    调两次第二次会因 if_key="WIFI_STA_DEF" 重复而返回 NULL。所以本函数必须幂等（上面已守卫）。 */
    s.sta = esp_netif_create_default_wifi_sta();
    if (!s.sta) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta 失败（WIFI_STA_DEF 已存在？）");
        return ESP_FAIL;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_hosted_event, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_hosted_event, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }

    if (!s.retry_timer) {
        const esp_timer_create_args_t targs = {
            .callback = net_hosted_retry_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "nh_retry",
        };
        err = esp_timer_create(&targs, &s.retry_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(err));
            return err;
        }
    }
#if CONFIG_NET_HOSTED_LOG_CP_VERSION
    net_hosted_log_cp_version();   /* 先留档：RPC 失败也不影响后面（无凭据也要能起） */
#endif

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s"
                      "（若是 ESP_ERR_NOT_SUPPORTED：检查 CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y —— "
                      "缺了会静默退回 weak 桩，见 feasibility §5.4）", esp_err_to_name(err));
        s.st.state = NET_HOSTED_STATE_FAILED;
        return err;
    }

    const char *ssid = CONFIG_NET_HOSTED_WIFI_SSID;
    const char *psk = CONFIG_NET_HOSTED_WIFI_PASSWORD;
    s.ssid_configured = (ssid[0] != '\0');

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        return err;
    }

    if (s.ssid_configured) {
        wifi_config_t wc;
        memset(&wc, 0, sizeof(wc));
        size_t n = strlen(ssid);
        if (n > sizeof(wc.sta.ssid)) {
            n = sizeof(wc.sta.ssid);
        }
        memcpy(wc.sta.ssid, ssid, n);
        n = strlen(psk);
        if (n > sizeof(wc.sta.password)) {
            n = sizeof(wc.sta.password);
        }
        memcpy(wc.sta.password, psk, n);
#if CONFIG_NET_HOSTED_WPA3_SAE
        wc.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#else
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#endif
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "STA 配置: SSID=\"%s\" auth=%s", ssid, NET_HOSTED_AUTH_STR);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        s.st.state = NET_HOSTED_STATE_FAILED;
        return err;
    }

    s.started = true;   /* 之后的重复调用直接 OK（重连定时器回调需要它） */

    if (!s.ssid_configured) {
        net_hosted_scan_only();
    }
    return ESP_OK;
}

esp_err_t net_hosted_stop(void)
{
    if (!s.started) {
        return ESP_OK;
    }
    if (s.retry_timer) {
        esp_timer_stop(s.retry_timer);
    }
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    s.started = false;
    s.st.connected = false;
    s.st.has_ip = false;
    s.st.state = NET_HOSTED_STATE_IDLE;
    return ESP_OK;
}

esp_netif_t *net_hosted_upstream_netif(void)
{
    return s.sta;
}

bool net_hosted_has_ip(void)
{
    return s.st.has_ip;
}

bool net_hosted_is_connected(void)
{
    return s.st.connected;
}

esp_err_t net_hosted_get_sta_mac(uint8_t mac[6])
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_get_mac(WIFI_IF_STA, mac);
}

void net_hosted_get_status(net_hosted_status_t *out)
{
    if (!out) {
        return;
    }
    *out = s.st;
}

uint32_t net_hosted_last_scan_count(void)
{
    return s.scan_count;
}
