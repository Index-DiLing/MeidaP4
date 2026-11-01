// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I1 复合层 —— **IDF 侧胶水**：安装 TinyUSB、BOS / MS OS 2.0 回调、
// vendor 供应商标控制请求分发、数据面、统计。
//
// 描述符的**组装与分配**全部在 `usb_composite_desc.c`（纯计算，可被原生 gcc 编译，
// 供 test/host 离线校验）。本文件只做"把组装结果交给 TinyUSB + 桥接数据"。
//
// 契约：docs/contracts/frozen-allocation.md · components/usb_composite/include/usb_composite.h
// 生产者：Lead

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tusb.h"
#include "class/vendor/vendor_device.h"

#include "usb_composite.h"
#include "usb_composite_internal.h"

static const char *TAG = "usb_composite";

/* =====================================================================
 * 0. 编译期前提（与冻结分配表一致，写错就编不过）
 * ===================================================================== */

#if !CFG_TUD_VENDOR
#error "usb_composite 需要 CFG_TUD_VENDOR >= 1（CONFIG_TINYUSB_VENDOR_COUNT）"
#endif

#if !(SOC_USB_OTG_PERIPH_NUM > 1)
#error "本固件按 ESP32-P4 双口芯片编写（HS 口 = TINYUSB_PORT_HIGH_SPEED_0）"
#endif

/* BOS 平台能力里的供应商请求码（frozen-allocation §5）——与分发严格对应 */
#define UC_WEBUSB_VENDOR_CODE  0x01
#define UC_MSOS_VENDOR_CODE    0x02
/* GET_MS_DESCRIPTOR 的 wIndex：MS 文档/Zephyr 用 7，本工程契约为 0 → 两者都接受 */
#define UC_MSOS_DESCRIPTOR_INDEX 0x07

_Static_assert(UC_WEBUSB_VENDOR_CODE != UC_MSOS_VENDOR_CODE,
               "WebUSB 与 MS OS 2.0 必须用不同的 bVendorCode，否则主机请求会打架");

/* =====================================================================
 * 1. 状态
 * ===================================================================== */

/** 一个 vendor 功能在 vendor 驱动里的槽位（ep 地址 → vendor_idx 的双向映射） */
typedef struct {
    uint8_t ep_in;    /**< 含 0x80；0 = 该功能没有 IN 端点 */
    uint8_t ep_out;   /**< 不含方向位；0 = 没有 OUT 端点 */
    uint8_t vendor_idx;
} uc_vendor_slot_t;

static uc_vendor_slot_t s_vendor[USB_COMPOSITE_MAX_FUNCTIONS];
static int s_vendor_count;
static bool s_started;
static bool s_user_identity;
static char s_serial[32];
static const char *s_str_ptrs[USB_COMPOSITE_MAX_STR];
static volatile usb_composite_stats_t s_stats;

/* =====================================================================
 * 2. 身份
 * ===================================================================== */

void usb_composite_identity_default(usb_composite_identity_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->vid = 0x303A;
    out->pid = 0x4103;   /* ⚠️ 刻意在 esp_tinyusb 自动 PID 空间（0x4000~0x407F）之外 */
    out->bcd_usb = 0x0210;
    /*
     * bcdDevice 0x0110（2026-10-22 T2）：
     *   本版把 IF1（视频）也加进了 MS OS 2.0 Function Subset。Windows 会把
     *   MS OS 描述符的结果按设备硬件 ID（含 REV）缓存，不改 REV 可能沿用旧结果
     *   ⇒ MI_01 仍然没有 winusb.sys。递增 bcdDevice 是让主机重新取 MSOS 描述符的最小改动。
     *   （PID 保持 0x4103 不变；若发现缓存仍不刷新，再考虑换 PID。）
     */
    out->bcd_device = 0x0110;
    out->b_device_class = 0xEF;   /* Misc */
    out->b_device_subclass = 0x02; /* Common */
    out->b_device_protocol = 0x01; /* IAD */
    out->self_powered = false;
    out->vbus_monitor_io = -1;
    out->max_power_ma = 500;
    out->manufacturer = NULL;   /* NULL = 用冻结默认值 */
    out->product = NULL;
    out->serial = NULL;         /* NULL = 由 eFuse MAC 生成 */
}

esp_err_t usb_composite_set_identity(const usb_composite_identity_t *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uc_is_prepared()) {
        ESP_LOGE(TAG, "set_identity 必须在 prepare 之前调用");
        return ESP_ERR_INVALID_STATE;
    }
    s_user_identity = true;
    return uc_set_identity(id);
}

/* =====================================================================
 * 3. 注册 / 组装 / 启动
 * ===================================================================== */

esp_err_t usb_composite_register(const usb_function_t *fn)
{
    esp_err_t err = uc_register(fn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register('%s') 失败: %s", fn && fn->name ? fn->name : "?",
                 uc_last_error());
    }
    return err;
}

/** 从组装结果建立 vendor_idx <-> ep 地址映射 */
static void build_vendor_map(void)
{
    s_vendor_count = 0;
    int n = uc_fn_count();
    for (int i = 0; i < n; i++) {
        const usb_fn_alloc_t *a = uc_fn_alloc(i);
        const usb_function_t *fn = uc_fn(i);
        if (a == NULL || fn == NULL || !fn->is_vendor) {
            continue;
        }
        if (s_vendor_count >= USB_COMPOSITE_MAX_FUNCTIONS) {
            break;
        }
        uc_vendor_slot_t *v = &s_vendor[s_vendor_count++];
        memset(v, 0, sizeof(*v));
        v->vendor_idx = a->vendor_idx;
        v->ep_in = (a->ep_in_count > 0) ? a->ep_in[0] : 0;
        v->ep_out = (a->ep_out_count > 0) ? a->ep_out[0] : 0;
    }
}

esp_err_t usb_composite_prepare(void)
{
    if (uc_is_prepared()) {
        return ESP_OK;
    }

    if (!s_user_identity) {
        /* 未显式设过身份 → 用冻结默认值，并把序列号从 eFuse MAC 生成（稳定、便于用户辨认） */
        usb_composite_identity_t id;
        usb_composite_identity_default(&id);
        uint8_t mac[6] = { 0 };
        if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
            snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            id.serial = s_serial;
        }
        esp_err_t err = uc_set_identity(&id);
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = uc_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prepare 失败 (%s): %s", esp_err_to_name(err), uc_last_error());
        return err;
    }

    build_vendor_map();

    int str_count = 0;
    const char *const *str = uc_str_desc(&str_count);
    if (str == NULL || str_count <= 0) {
        ESP_LOGE(TAG, "prepare: 字符串描述符为空");
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < str_count; i++) {
        s_str_ptrs[i] = str[i];
    }

    const usb_composite_budget_t *b = uc_budget();
    ESP_LOGI(TAG,
             "组装完成 | IF=%u FN=%u(vendor %u) | IN=%u OUT=%u | FS cfg=%" PRIu32 " B"
             " HS cfg=%" PRIu32 " B | BOS=%" PRIu32 " B | MSOS=%" PRIu32 " B | str=%" PRIu32,
             b->itf_total, b->fn_total, b->vendor_fn_total, b->ep_in_total, b->ep_out_total,
             b->fs_cfg_len, b->hs_cfg_len, b->bos_len, b->msos_len, b->str_count);
    ESP_LOGI(TAG, "DWC2 FIFO 预算 | used=%u word / %u word (free %u) —— 见 frozen-allocation §2.2",
             b->fifo_words_used, b->fifo_words_total, b->fifo_words_free);

    if (s_vendor_count != CFG_TUD_VENDOR) {
        /* 描述符里的 vendor 接口数必须与 esp_tinyusb 编进固件的 vendor 驱动槽位数严格一致，
         * 否则 vendor 功能会拿不到自己的 tx/rx stream（或多余槽位永远不 mounted）。 */
        ESP_LOGE(TAG, "vendor 功能数 %d 与 CFG_TUD_VENDOR(%d) 不一致 —— "
                      "请把 CONFIG_TINYUSB_VENDOR_COUNT 设为 %d",
                 s_vendor_count, (int)CFG_TUD_VENDOR, s_vendor_count);
        return ESP_ERR_INVALID_STATE;
    }

    /* ========================================================================
     * 调用各功能的 init(alloc) 回调 —— I1 契约要求："号码已分配、TinyUSB 尚未 install 时调用"。
     *
     * ⚠️ 2026-10-21 真机发现漏了这一步：遥测里 `Vendor: ep_bound=0`，
     *    意味着 usb_vendor 的两个功能**从未拿到 usb_fn_alloc_t** ⇒ 不知道 EP3/EP4 是哪个 ⇒
     *    视频/控制数据面根本没接通（即使 video_pipe 有帧也发不出去）。
     *    典型症状是"编译链接全绿、枚举也成功、但功能静默不工作"——又一例"看起来通了其实没接"。
     *
     * 顺序：在 uc_prepare()（号码已分配）之后、tinyusb_driver_install()（start() 里）之前。
     * 失败策略：**记日志但不中断**——某个功能的 init 失败不应把整个复合设备拖死，
     *          与 probe 阶段"失败不要 abort"的结论一致（probe-report §2.3）。
     * ====================================================================== */
    {
        int n = uc_fn_count();
        for (int i = 0; i < n; i++) {
            const usb_function_t *fn = uc_fn(i);
            const usb_fn_alloc_t *a = uc_fn_alloc(i);
            if (fn == NULL || a == NULL) {
                continue;
            }
            ESP_LOGI(TAG,
                     "fn init: '%s' | itf=[%u..%u] in=%u out=%u vendor_idx=%u str=%u/%u",
                     fn->name ? fn->name : "?", a->itf[0],
                     a->itf_count ? (unsigned)(a->itf[0] + a->itf_count - 1) : a->itf[0],
                     a->ep_in_count, a->ep_out_count, a->vendor_idx,
                     a->itf_str_idx, a->extra_str_idx);
            if (fn->init != NULL) {
                esp_err_t ierr = fn->init(a);
                if (ierr != ESP_OK) {
                    ESP_LOGE(TAG, "fn init '%s' 失败 (%s) —— 继续，其它功能不受影响",
                             fn->name ? fn->name : "?", esp_err_to_name(ierr));
                }
            }
        }
    }

    return ESP_OK;
}

static void composite_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_stats.mounted = true;
        s_stats.mount_count++;
        ESP_LOGI(TAG, "USB 已挂载（主机枚举完成）");
        break;
    case TINYUSB_EVENT_DETACHED:
        s_stats.mounted = false;
        s_stats.unmount_count++;
        ESP_LOGW(TAG, "USB 已断开");
        break;
    default:
        break;
    }
}

esp_err_t usb_composite_start(void)
{
    if (s_started) {
        ESP_LOGE(TAG, "已经启动过");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = usb_composite_prepare();
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_USB_COMPOSITE_SELFCHECK_ON_BOOT
    uint32_t run = 0, failed = 0;
    err = uc_selfcheck(&run, &failed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "描述符自检 FAIL：%u/%u 条失败 —— 首个问题：%s", failed, run, uc_last_error());
        return err;
    }
    ESP_LOGI(TAG, "描述符自检 PASS（%u 条）", run);
#endif

    tinyusb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = TINYUSB_PORT_HIGH_SPEED_0;   /* ⚠️ P4 双口芯片：不设会走 FS 口（放不下 NCM） */
    /* 注意：bool 且 default n 的 Kconfig 项在 sdkconfig.h 里**没有** #define，
     * 直接写 CONFIG_XXX 会报 undeclared —— 必须用 defined() 守卫。 */
#if defined(CONFIG_USB_COMPOSITE_SELF_POWERED) && CONFIG_USB_COMPOSITE_SELF_POWERED
    cfg.phy.self_powered = true;
#else
    cfg.phy.self_powered = false;
#endif
    cfg.phy.vbus_monitor_io = CONFIG_USB_COMPOSITE_VBUS_MONITOR_IO;
    cfg.phy.skip_setup = false;
    cfg.task.size = 4096;
    cfg.task.priority = 5;
    /* ⚠️ **不能**用 `tskNO_AFFINITY`（Lead 2026-10-21 真机实测）：
     *   esp_tinyusb 2.3.0 的 `tinyusb_task_check_config()`（tinyusb_task.c:109）判据是
     *     `config->xCoreID <= SOC_CPU_CORES_NUM`
     *   而 `tskNO_AFFINITY` = 0x7FFFFFFF ≫ 2 ⇒ `tinyusb_driver_install()` 返回
     *   ESP_ERR_INVALID_ARG，**整个复合设备一个都装不起来**（串口报
     *   "Task affinity should be less or equal to CPU amount"）。
     *   P4 是双核：合法值是 0 或 1。这里钉到 **CPU1**（与探测期 USB 任务同核，已验证可用），
     *   把 CPU0 留给应用/视频。 */
    cfg.task.xCoreID = 1;
    cfg.event_cb = composite_event_cb;
    cfg.event_arg = NULL;

    cfg.descriptor.device = (const tusb_desc_device_t *)uc_dev_desc(NULL);
    cfg.descriptor.qualifier = (const tusb_desc_device_qualifier_t *)uc_qual_desc(NULL);
    cfg.descriptor.string = s_str_ptrs;
    cfg.descriptor.string_count = (int)uc_budget()->str_count;
    cfg.descriptor.full_speed_config = uc_cfg_desc(false, NULL);
    cfg.descriptor.high_speed_config = uc_cfg_desc(true, NULL);

    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "TinyUSB 已安装（HS 口 | FS cfg %" PRIu32 " B / HS cfg %" PRIu32
                  " B | BOS %" PRIu32 " B | MSOS %" PRIu32 " B）",
             uc_budget()->fs_cfg_len, uc_budget()->hs_cfg_len, uc_budget()->bos_len,
             uc_budget()->msos_len);
    return ESP_OK;
}

bool usb_composite_is_mounted(void) { return s_stats.mounted; }

void usb_composite_get_stats(usb_composite_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    /* 逐字段拷贝：volatile 结构体不能直接整体赋值 */
    out->mounted = s_stats.mounted;
    out->mount_count = s_stats.mount_count;
    out->unmount_count = s_stats.unmount_count;
    out->ctrl_requests = s_stats.ctrl_requests;
    out->ctrl_stalls = s_stats.ctrl_stalls;
    out->webusb_get_url = s_stats.webusb_get_url;
    out->msos_requests = s_stats.msos_requests;
}

/* =====================================================================
 * 4. TinyUSB 描述符回调
 * ===================================================================== */

/** BOS：esp_tinyusb 2.3.0 不提供 `tud_descriptor_bos_cb()` 的强符号（只提供 device/qualifier/string），
 *  所以 BOS 必须由本组件提供 —— WebUSB 平台能力就在这里。 */
uint8_t const *tud_descriptor_bos_cb(void)
{
    uint32_t len = 0;
    const uint8_t *bos = uc_bos_desc(&len);
    if (bos == NULL || len == 0) {
        ESP_LOGE(TAG, "tud_descriptor_bos_cb: BOS 未准备好");
        return NULL;
    }
    return bos;
}

/* =====================================================================
 * 5. 供应商控制请求分发
 * ===================================================================== */

/** 找 wIndex（接口号）对应的功能下标；找不到返回 -1 */
static int fn_by_interface(uint16_t wIndex)
{
    int n = uc_fn_count();
    for (int i = 0; i < n; i++) {
        const usb_fn_alloc_t *a = uc_fn_alloc(i);
        if (a == NULL) {
            continue;
        }
        if (wIndex >= a->itf[0] && wIndex < (uint16_t)(a->itf[0] + a->itf_count)) {
            return i;
        }
    }
    return -1;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (request == NULL) {
        return false;
    }
    /* 只处理"供应商"类型的请求；标准请求留给 TinyUSB 自己（TUSB_REQ_TYPE_VENDOR == 2） */
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    s_stats.ctrl_requests++;

    /* --- WebUSB GET_URL（bRequest == 0x01）---
     * 我们的 BOS 里 iLandingPage = 0，Chrome 正常**不会**发这个请求（批 0 真机确认）。
     * 真发过来说明有主机在按非 0 着陆页处理，我们没有着陆页 → stall，并计数以便诊断。 */
    if (request->bRequest == UC_WEBUSB_VENDOR_CODE) {
        s_stats.webusb_get_url++;
        s_stats.ctrl_stalls++;
        ESP_LOGW(TAG, "收到 WebUSB GET_URL（应有 iLandingPage=0，不该发生）—— stall");
        return false;
    }

    /* --- MS OS 2.0 GET_MS_DESCRIPTOR（bRequest == 0x02）---
     * ⚠️ wIndex 必须同时接受 0 与 0x07：Windows 实测发的是 0x0007（probe-report §2.2）。 */
    if (request->bRequest == UC_MSOS_VENDOR_CODE) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (request->wIndex != 0 && request->wIndex != UC_MSOS_DESCRIPTOR_INDEX) {
            s_stats.ctrl_stalls++;
            ESP_LOGW(TAG, "MS OS 2.0 请求 wIndex=0x%04X 不在 {0, 0x07} 内 —— stall",
                     request->wIndex);
            return false;
        }
        uint32_t len = 0;
        const uint8_t *msos = uc_msos_desc(&len);
        if (msos == NULL || len == 0) {
            s_stats.ctrl_stalls++;
            return false;
        }
        s_stats.msos_requests++;
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)msos, (uint16_t)len);
    }

    /* --- 其余供应商请求 → 转给拥有该接口的功能 --- */
    int idx = fn_by_interface(request->wIndex);
    if (idx >= 0) {
        const usb_function_t *fn = uc_fn(idx);
        if (fn != NULL && fn->on_control != NULL) {
            usb_setup_packet_t pkt;
            pkt.bmRequestType = request->bmRequestType;
            pkt.bRequest = request->bRequest;
            pkt.wValue = request->wValue;
            pkt.wIndex = request->wIndex;
            pkt.wLength = request->wLength;
            if (fn->on_control(&pkt, fn->user_ctx)) {
                return true;
            }
        }
    }

    s_stats.ctrl_stalls++;
    ESP_LOGW(TAG, "未处理的供应商请求：bRequest=0x%02X wIndex=0x%04X wLength=%u",
             request->bRequest, request->wIndex, request->wLength);
    return false;
}

/* =====================================================================
 * 6. 数据面（ep 地址 <-> vendor 驱动槽位）
 * ===================================================================== */

static uc_vendor_slot_t *slot_by_ep_in(uint8_t ep)
{
    for (int i = 0; i < s_vendor_count; i++) {
        if (s_vendor[i].ep_in != 0 && s_vendor[i].ep_in == ep) {
            return &s_vendor[i];
        }
    }
    return NULL;
}

static uc_vendor_slot_t *slot_by_ep_out(uint8_t ep)
{
    for (int i = 0; i < s_vendor_count; i++) {
        if (s_vendor[i].ep_out != 0 && s_vendor[i].ep_out == ep) {
            return &s_vendor[i];
        }
    }
    return NULL;
}

size_t usb_composite_ep_write_available(uint8_t ep_in_addr)
{
    uc_vendor_slot_t *v = slot_by_ep_in(ep_in_addr);
    if (v == NULL || !s_started) {
        return 0;
    }
    return (size_t)tud_vendor_n_write_available(v->vendor_idx);
}

size_t usb_composite_ep_write_pending(uint8_t ep_in_addr)
{
    uc_vendor_slot_t *v = slot_by_ep_in(ep_in_addr);
    if (v == NULL || !s_started) {
        return 0;
    }
    /*
     * ⚠️ 不能用"自己记的合成计数器"（2026-10-22 T2 真机实测的阻塞 bug）：
     * esp_tinyusb 的 vendor 驱动是 **FIFO + 完成回调里自动续传**（vendord_xfer_cb 在
     * TX complete 时自己调 tu_edpt_stream_write_xfer() 把 FIFO 里的字节接着发出去）。
     * 也就是说 FIFO 会被驱动**异步**排空，而主机侧只看到"我调了 flush，但没返回字节数"。
     * 旧实现维护 pending = accepted - flushed，于是：
     *   端点忙时 flush 返回 0 → pending 变大；之后驱动自动续传把 FIFO 排空 →
     *   下次 flush 因 FIFO 已空而返回 0 ⇒ pending 永远减不下去 ⇒ 所有后续写返回 0
     *   ⇒ 设备从此再也发不出任何一帧（真机表现：EP4 只出 HELLO/PING 两帧，之后静默）。
     * 正确口径：**直接用驱动里的 FIFO 占用**（TX_BUFSIZE - write_available），
     * 它由驱动自己维护，天然不会与自动续传打架。
     */
    return (size_t)(CFG_TUD_VENDOR_TX_BUFSIZE - tud_vendor_n_write_available(v->vendor_idx));
}

size_t usb_composite_ep_write(uint8_t ep_in_addr, const void *data, size_t len)
{
    uc_vendor_slot_t *v = slot_by_ep_in(ep_in_addr);
    uint32_t accepted;
    size_t avail;

    if (v == NULL || !s_started || data == NULL || len == 0) {
        return 0;
    }

    /* FIFO 里没空间（或端点正忙）就先让驱动尝试排空，本次不收 —— 调用方按背压重试 */
    avail = (size_t)tud_vendor_n_write_available(v->vendor_idx);
    if (avail == 0) {
        (void)tud_vendor_n_write_flush(v->vendor_idx);
        return 0;
    }
    if (len > avail) {
        len = avail;   /* 收下多少算多少；调用方（wu_write_all / 视频任务）负责续写 */
    }

    accepted = tud_vendor_n_write(v->vendor_idx, data, (uint32_t)len);
    if (accepted == 0) {
        return 0;
    }
    /* 尽量立刻推出去；推不完没关系 —— 完成回调会把 FIFO 里剩下的接着发 */
    (void)tud_vendor_n_write_flush(v->vendor_idx);
    return (size_t)accepted;
}

size_t usb_composite_ep_read_available(uint8_t ep_out_addr)
{
    uc_vendor_slot_t *v = slot_by_ep_out(ep_out_addr);
    if (v == NULL || !s_started) {
        return 0;
    }
    return (size_t)tud_vendor_n_available(v->vendor_idx);
}

size_t usb_composite_ep_read(uint8_t ep_out_addr, void *buf, size_t cap)
{
    uc_vendor_slot_t *v = slot_by_ep_out(ep_out_addr);
    if (v == NULL || !s_started || buf == NULL || cap == 0) {
        return 0;
    }
    return (size_t)tud_vendor_n_read(v->vendor_idx, buf, (uint32_t)cap);
}

/* OUT 数据到达通知。
 * 签名跟随 TinyUSB 的兼容开关（本工程 CFG_TUD_API_V0_19_COMPAT 默认为 1 → bufsize 是 uint16_t）。 */
#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
#else
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize)
#endif
{
    (void)buffer;
    (void)bufsize;
    for (int i = 0; i < s_vendor_count; i++) {
        uc_vendor_slot_t *v = &s_vendor[i];
        if (v->vendor_idx != idx) {
            continue;
        }
        int fn_idx = uc_fn_by_vendor_idx(v->vendor_idx);
        if (fn_idx < 0) {
            return;
        }
        const usb_function_t *fn = uc_fn(fn_idx);
        if (fn != NULL && fn->on_out != NULL && v->ep_out != 0) {
            fn->on_out(v->ep_out, fn->user_ctx);
        }
        return;
    }
}

/* =====================================================================
 * 7. 预算 / 自检 / dump
 * ===================================================================== */

esp_err_t usb_composite_get_budget(usb_composite_budget_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = usb_composite_prepare();
    if (err != ESP_OK) {
        return err;
    }
    *out = *uc_budget();
    return ESP_OK;
}

esp_err_t usb_composite_selfcheck(void)
{
    uint32_t run = 0, failed = 0;
    esp_err_t err = uc_selfcheck(&run, &failed);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "自检 PASS（%u 条）", run);
    } else {
        ESP_LOGE(TAG, "自检 FAIL：%u/%u 条失败 —— 首个问题：%s", failed, run, uc_last_error());
    }
    return err;
}

const uint8_t *usb_composite_config_desc(bool high_speed, uint32_t *len)
{
    return uc_cfg_desc(high_speed, len);
}

const uint8_t *usb_composite_bos_desc(uint32_t *len) { return uc_bos_desc(len); }
const uint8_t *usb_composite_msos_desc(uint32_t *len) { return uc_msos_desc(len); }
const uint8_t *usb_composite_device_desc(uint32_t *len) { return uc_dev_desc(len); }
