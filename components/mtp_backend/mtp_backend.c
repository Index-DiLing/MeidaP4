// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · MTP 后端（WS-storage）
//
// 做什么：
//   1. 幂等地确保 SD 卷已挂载（components/sd_storage，挂载点只有一处定义）。
//   2. `tinyusb_mtp_install_driver()` + `tinyusb_mtp_register_storage()`：把 SD 卡注册成
//      **唯一**卷（frozen-allocation §1：MTP 只暴露 SD 卡一个卷，不暴露片内 storage 分区）。
//   3. 实现 I1 的 `usb_function_t` 并 `usb_composite_register()`：
//      IF 类 06/01/01、3 个端点（事件 interrupt IN 64B/1ms + 数据 bulk OUT + 数据 bulk IN，
//      FS 64 / HS 512）、`wcid = false`（MTP 那一路由 Windows 自带 wpdmtp.sys 按接口类绑定，
//      **不发** CompatibleID / Registry Property —— frozen-allocation §5）。
//
// 三条硬性约束（来源都写在注释里，别删）：
//   A. 描述符里的接口号/端点号**只能**取自 `usb_fn_alloc_t`，不得硬编码（I1 契约）。
//   B. `ep_out_same_num_as_in` 必须**整表**初始化：没写到的元素是 0，会被解释成"复用 IN[0]"，
//      把 MTP 的 bulk OUT 挂到事件端点上（Lead 明确警告过）。
//   C. `-Wl,--undefined=mtp_dispatch` 必须在 CMakeLists 里（见该文件的注释）：esp_tinyusb 2.3.0
//      打包缺陷会让 MTP 的**弱默认**回调胜出，真机表现为"枚举成功但所有 MTP 命令 stall"
//      （probe-report §3.2）。
//
// 生产者：WS-storage。写作用域：components/mtp_backend/**。

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "sd_storage.h"
#include "usb_composite.h"

/* TinyUSB 的通用常量（TUSB_DESC_xxx / TUSB_XFER_xxx / TUSB_CLASS_IMAGE / TUD_MTP_DESC_LEN）
   与 MTP 是否启用无关，所以无条件包含；MTP 类自己的头（tinyusb_mtp.h）才有开关。 */
#include "tusb.h"

#include "mtp_backend.h"

#if CONFIG_TINYUSB_MTP_ENABLED
#include "tinyusb_mtp.h"   // tinyusb_mtp_install_driver / tinyusb_mtp_register_storage
#endif

// ---------------------------------------------------------------------------
// Kconfig 兜底
// ---------------------------------------------------------------------------
#ifndef CONFIG_MTP_BACKEND_MANUFACTURER
#define CONFIG_MTP_BACKEND_MANUFACTURER "MeidaP4"
#endif
#ifndef CONFIG_MTP_BACKEND_MODEL
#define CONFIG_MTP_BACKEND_MODEL "MeidaP4 SD Card"
#endif
#ifndef CONFIG_MTP_BACKEND_VERSION
#define CONFIG_MTP_BACKEND_VERSION "1.0.0"
#endif
#ifndef CONFIG_MTP_BACKEND_STORAGE_NAME
#define CONFIG_MTP_BACKEND_STORAGE_NAME "MeidaP4 SD Card"
#endif

static const char *TAG = "mtp_backend";

/* ===========================================================================
 * 1. 冻结的参数（唯一来源；描述符与注册表共用同一组宏，不会各写各的）
 * ========================================================================= */

/** MTP 描述符片段 = Interface(9) + 事件 IN(7) + 数据 OUT(7) + 数据 IN(7) = 30 B */
#define MTP_ITF_DESC_LEN   9u
#define MTP_EP_DESC_LEN    7u
#define MTP_FN_DESC_LEN    (MTP_ITF_DESC_LEN + 3u * MTP_EP_DESC_LEN)

/** 端点需求（frozen-allocation §2：EP1 interrupt IN 64/1ms、EP2 bulk IN+OUT 512(HS)/64(FS)） */
#define MTP_EP_EVT_MPS        64u
#define MTP_EP_EVT_INTERVAL   1u
#define MTP_EP_BULK_MPS_HS    512u
#define MTP_EP_BULK_MPS_FS    64u

#define MTP_EP_IN_COUNT       2u
#define MTP_EP_OUT_COUNT      1u

/**
 * 第 0 个 OUT 复用**第 1 个 IN** 的号码（⇒ 数据 IN 与数据 OUT 同号 = EP2）。
 * 这就是 Lead 把隐式配对换成显式表的原因：MTP 有 2 个 IN 但只有 1 个 OUT。
 */
#define MTP_OUT0_PAIRS_IN_INDEX  1u

_Static_assert(MTP_EP_BULK_MPS_HS == 512u && MTP_EP_BULK_MPS_FS == 64u && MTP_EP_EVT_MPS == 64u,
               "端点 wMaxPacketSize 与冻结表不一致（frozen-allocation §2）");
_Static_assert(MTP_EP_IN_COUNT == 2u && MTP_EP_OUT_COUNT == 1u,
               "MTP = 1 个中断 IN + 1 对 bulk 数据端点");
_Static_assert(USB_COMPOSITE_MAX_EP_PER_DIR == 7u,
               "ep_out_same_num_as_in[] 必须整表初始化；若 MAX_EP_PER_DIR 变化，"
               "请把下面的表补满 USB_EP_NO_PAIR 并同步改这个断言");

/* ---- 状态（两个构建形态都要有，供查询函数使用） ---- */
static bool s_driver_installed = false;
static bool s_fn_registered = false;
static bool s_storage_registered = false;

#if CONFIG_TINYUSB_MTP_ENABLED

#if MTP_FN_DESC_LEN != TUD_MTP_DESC_LEN
#error "MTP 描述符片段长度必须等于 TinyUSB 的 TUD_MTP_DESC_LEN (9+7+7+7)"
#endif

/* ===========================================================================
 * 2. 参考模板：TinyUSB 自己的 TUD_MTP_DESCRIPTOR，用于把运行时片段钉死在上面
 *
 * 用一组**与真实分配值不同且互不重复**的占位号生成参考字节，然后拿一个"假 alloc"
 * 调我们自己的 build_desc 逐字节比对（见 mtp_backend_desc_selfcheck）。
 * 这样"我拼出来的 30 字节"与"上游模板的 30 字节"由机器证明相同，不靠人眼。
 * ========================================================================= */
#define MTP_REF_ITF        0x11u
#define MTP_REF_STR        0x22u
#define MTP_REF_EVT_IN     (0x80u | 0x33u)
#define MTP_REF_BULK_OUT   0x44u
#define MTP_REF_BULK_IN    (0x80u | 0x55u)
/* 故意与 MTP_EP_*_MPS 分开写：这样下面这条断言真的在"交叉校验两边是否一致" */
#define MTP_REF_MPS_HS     512u
#define MTP_REF_MPS_FS     64u
#define MTP_REF_MPS_EVT    64u

_Static_assert(MTP_REF_MPS_HS == MTP_EP_BULK_MPS_HS &&
               MTP_REF_MPS_FS == MTP_EP_BULK_MPS_FS &&
               MTP_REF_MPS_EVT == MTP_EP_EVT_MPS,
               "参考模板与冻结参数必须用同样的 wMaxPacketSize");

/* ⚠️ 顺序（interrupt IN → bulk OUT → bulk IN）是 **mtpd_open() 的硬要求**：
   mtp_device.c:296-303 依次把第一个端点当 interrupt IN 打开，再把后两个当一对 bulk 打开。
   改这个顺序 = MTP 直接打不开。 */
static const uint8_t s_mtp_ref_hs[TUD_MTP_DESC_LEN] = {
    TUD_MTP_DESCRIPTOR(MTP_REF_ITF, MTP_REF_STR, MTP_REF_EVT_IN, MTP_REF_MPS_EVT, 1u,
                       MTP_REF_BULK_OUT, MTP_REF_BULK_IN, MTP_REF_MPS_HS)
};
static const uint8_t s_mtp_ref_fs[TUD_MTP_DESC_LEN] = {
    TUD_MTP_DESCRIPTOR(MTP_REF_ITF, MTP_REF_STR, MTP_REF_EVT_IN, MTP_REF_MPS_EVT, 1u,
                       MTP_REF_BULK_OUT, MTP_REF_BULK_IN, MTP_REF_MPS_FS)
};

/* ===========================================================================
 * 3. 注册描述（I1）
 * ========================================================================= */

static esp_err_t mtp_build_desc(bool high_speed, const usb_fn_alloc_t *alloc,
                                uint8_t *buf, size_t cap, size_t *out_len);
static esp_err_t mtp_fn_init(const usb_fn_alloc_t *alloc);
static void mtp_fn_on_mount(bool mounted, void *user_ctx);

static const usb_function_t s_mtp_fn = {
    .name = "mtp",
    .itf_count = 1,
    /* IF0 的 iInterface = "MeidaP4 SD Card"（frozen-allocation §4 idx 5）。
       同一个符号也用作 Windows 侧友好名与 MTP 存储名，保证三处一致。 */
    .itf_string = CONFIG_MTP_BACKEND_STORAGE_NAME,
    .extra_string = NULL,

    /* 单接口功能**不加 IAD**（frozen-allocation §6：单接口 IAD 非法且会干扰 usbccgp 分发） */
    .use_iad = false,

    .ep_in_req = {
        [0] = { .xfer = USB_EP_XFER_INTERRUPT, .mps_hs = MTP_EP_EVT_MPS,     .mps_fs = MTP_EP_EVT_MPS,     .interval = MTP_EP_EVT_INTERVAL },
        [1] = { .xfer = USB_EP_XFER_BULK,      .mps_hs = MTP_EP_BULK_MPS_HS, .mps_fs = MTP_EP_BULK_MPS_FS, .interval = 0 },
    },
    .ep_in_count = MTP_EP_IN_COUNT,
    .ep_out_req = {
        [0] = { .xfer = USB_EP_XFER_BULK,      .mps_hs = MTP_EP_BULK_MPS_HS, .mps_fs = MTP_EP_BULK_MPS_FS, .interval = 0 },
    },
    .ep_out_count = MTP_EP_OUT_COUNT,
    /* ⚠️ **整表**初始化（7 项），未列出的元素默认是 0 = "复用 IN[0]"，会把 bulk OUT
       挂到事件 interrupt 端点上。见 I1 头文件里该字段的注释。 */
    .ep_out_same_num_as_in = {
        MTP_OUT0_PAIRS_IN_INDEX,
        USB_EP_NO_PAIR,
        USB_EP_NO_PAIR,
        USB_EP_NO_PAIR,
        USB_EP_NO_PAIR,
        USB_EP_NO_PAIR,
        USB_EP_NO_PAIR,
    },

    .is_vendor = false,

    .build_desc = mtp_build_desc,
    .init = mtp_fn_init,

    /* MTP 的数据面（bulk IN/OUT）由 esp_tinyusb 的 MTP 类驱动自己收发
       （tud_mtp_* 回调 + 内部缓冲），**不能**再由复合层的 on_out 去
       usb_composite_ep_read() 排空 —— 那会把 MTP 的包抢走。所以这里必须是 NULL。 */
    .on_out = NULL,
    .on_mount = mtp_fn_on_mount,
    .on_control = NULL,
    .user_ctx = NULL,

    /* MTP 是受保护类（Still Image），Windows 用自带 wpdmtp.sys 按接口类绑定，
       **不发** CompatibleID / Registry Property（frozen-allocation §5）。 */
    .wcid = false,
    .wcid_compatible_id = NULL,
    .wcid_device_interface_guid = NULL,
};

/** `usb_ep_req_t.xfer` → 描述符 `bmAttributes` 里的传输类型 */
static inline uint8_t mtp_xfer_to_attr(usb_ep_xfer_t xfer)
{
    switch (xfer) {
    case USB_EP_XFER_INTERRUPT:
        return (uint8_t)TUSB_XFER_INTERRUPT;
    case USB_EP_XFER_BULK:
    default:
        return (uint8_t)TUSB_XFER_BULK;
    }
}

/**
 * @brief 生成 MTP 的接口 + 3 个端点描述符（30 B）。
 *
 * 号**全部**取自 `alloc`；速度只影响 bulk 的 wMaxPacketSize（FS 硬上限 64）。
 * 纯计算，可被离线自检反复调用。
 */
static esp_err_t mtp_build_desc(bool high_speed, const usb_fn_alloc_t *alloc,
                                uint8_t *buf, size_t cap, size_t *out_len)
{
    if (alloc == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

    if (alloc->itf_count < 1 || alloc->ep_in_count < MTP_EP_IN_COUNT ||
        alloc->ep_out_count < MTP_EP_OUT_COUNT) {
        ESP_LOGE(TAG, "allocation incomplete: itf=%u in=%u out=%u",
                 alloc->itf_count, alloc->ep_in_count, alloc->ep_out_count);
        return ESP_ERR_INVALID_STATE;
    }
    if (cap < MTP_FN_DESC_LEN) {
        ESP_LOGE(TAG, "descriptor buffer too small: %u < %u", (unsigned)cap, (unsigned)MTP_FN_DESC_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t ep_evt_in   = alloc->ep_in[0];    /* 第 0 个 IN = 事件 interrupt IN */
    const uint8_t ep_bulk_in  = alloc->ep_in[1];    /* 第 1 个 IN = 数据 bulk IN */
    const uint8_t ep_bulk_out = alloc->ep_out[0];   /* 第 0 个 OUT = 数据 bulk OUT */

    /* 方向位由复合层负责；这里只做防御性检查（不猜、不补方向位） */
    if ((ep_evt_in & 0x80u) == 0 || (ep_bulk_in & 0x80u) == 0 || (ep_bulk_out & 0x80u) != 0) {
        ESP_LOGE(TAG, "suspicious endpoint addresses from the composite layer: "
                      "evt_in=0x%02X bulk_in=0x%02X bulk_out=0x%02X",
                 ep_evt_in, ep_bulk_in, ep_bulk_out);
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t mps_bulk = high_speed ? MTP_EP_BULK_MPS_HS : MTP_EP_BULK_MPS_FS;

    size_t o = 0;

    /* ---- ① Interface：Still Image / PIMA 15470，3 个端点 ---- */
    buf[o++] = (uint8_t)MTP_ITF_DESC_LEN;
    buf[o++] = TUSB_DESC_INTERFACE;
    buf[o++] = alloc->itf[0];
    buf[o++] = 0;                                    /* bAlternateSetting */
    buf[o++] = 3;                                    /* bNumEndpoints：mtpd_open() 硬要求 3 */
    buf[o++] = TUSB_CLASS_IMAGE;                     /* 0x06 */
    buf[o++] = (uint8_t)MTP_SUBCLASS_STILL_IMAGE;    /* 0x01 */
    buf[o++] = (uint8_t)MTP_PROTOCOL_PIMA_15470;     /* 0x01 */
    buf[o++] = alloc->itf_str_idx;                   /* iInterface（复合层分配） */

    /* ---- ② Endpoint：事件 interrupt IN ---- */
    buf[o++] = (uint8_t)MTP_EP_DESC_LEN;
    buf[o++] = TUSB_DESC_ENDPOINT;
    buf[o++] = ep_evt_in;
    buf[o++] = mtp_xfer_to_attr(s_mtp_fn.ep_in_req[0].xfer);
    buf[o++] = (uint8_t)(MTP_EP_EVT_MPS & 0xFFu);
    buf[o++] = (uint8_t)(MTP_EP_EVT_MPS >> 8);
    buf[o++] = (uint8_t)MTP_EP_EVT_INTERVAL;

    /* ---- ③ Endpoint：数据 bulk OUT ---- */
    buf[o++] = (uint8_t)MTP_EP_DESC_LEN;
    buf[o++] = TUSB_DESC_ENDPOINT;
    buf[o++] = ep_bulk_out;
    buf[o++] = mtp_xfer_to_attr(s_mtp_fn.ep_out_req[0].xfer);
    buf[o++] = (uint8_t)(mps_bulk & 0xFFu);
    buf[o++] = (uint8_t)(mps_bulk >> 8);
    buf[o++] = 0;

    /* ---- ④ Endpoint：数据 bulk IN ---- */
    buf[o++] = (uint8_t)MTP_EP_DESC_LEN;
    buf[o++] = TUSB_DESC_ENDPOINT;
    buf[o++] = ep_bulk_in;
    buf[o++] = mtp_xfer_to_attr(s_mtp_fn.ep_in_req[1].xfer);
    buf[o++] = (uint8_t)(mps_bulk & 0xFFu);
    buf[o++] = (uint8_t)(mps_bulk >> 8);
    buf[o++] = 0;

    *out_len = o;
    return ESP_OK;
}

/** 号码分配完成后的回调：只做日志与一致性检查，不碰 USB 硬件、不读端点 */
static esp_err_t mtp_fn_init(const usb_fn_alloc_t *alloc)
{
    if (alloc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "I1 allocation: itf=%u (iInterface str idx %u) | "
                  "event IN=0x%02X interval=%u | bulk IN=0x%02X | bulk OUT=0x%02X",
             alloc->itf[0], alloc->itf_str_idx,
             alloc->ep_in[0], (unsigned)MTP_EP_EVT_INTERVAL,
             alloc->ep_in[1], alloc->ep_out[0]);

    /* 下面两条是**契约检查**，不是硬编码号：只看"关系"对不对，
       所以复合层怎么编号都不影响。 */
    if ((unsigned)(alloc->ep_out[0] & 0x7Fu) != (unsigned)(alloc->ep_in[MTP_OUT0_PAIRS_IN_INDEX] & 0x7Fu)) {
        ESP_LOGW(TAG, "bulk OUT 0x%02X 没有复用 bulk IN 0x%02X 的号码："
                      "ep_out_same_num_as_in[0]=%u 没被复合层兑现（I1 配对表语义）",
                 alloc->ep_out[0], alloc->ep_in[MTP_OUT0_PAIRS_IN_INDEX],
                 (unsigned)MTP_OUT0_PAIRS_IN_INDEX);
    }
    if ((unsigned)(alloc->ep_in[0] & 0x7Fu) == (unsigned)(alloc->ep_in[1] & 0x7Fu)) {
        ESP_LOGE(TAG, "两个 IN 端点拿到同一个号码（事件 0x%02X / 数据 0x%02X）——"
                      "描述符会重复，MTP 必然打不开", alloc->ep_in[0], alloc->ep_in[1]);
    }
    return ESP_OK;
}

static void mtp_fn_on_mount(bool mounted, void *user_ctx)
{
    (void)user_ctx;
    if (mounted) {
        ESP_LOGI(TAG, "host mounted the composite device: MTP storage %s",
                 mtp_backend_is_storage_registered()
                     ? "registered (SD card browseable)"
                     : "NOT registered (SD volume was unavailable at boot)");
    } else {
        ESP_LOGI(TAG, "host unmounted / disconnected");
    }
}

bool mtp_backend_desc_selfcheck(void)
{
    /* 与参考模板相同的占位号 */
    usb_fn_alloc_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.itf_count = 1;
    fake.itf[0] = MTP_REF_ITF;
    fake.itf_str_idx = MTP_REF_STR;
    fake.ep_in_count = MTP_EP_IN_COUNT;
    fake.ep_in[0] = MTP_REF_EVT_IN;
    fake.ep_in[1] = MTP_REF_BULK_IN;
    fake.ep_out_count = MTP_EP_OUT_COUNT;
    fake.ep_out[0] = MTP_REF_BULK_OUT;

    bool ok = true;
    for (int hs = 1; hs >= 0; hs--) {
        const bool high_speed = (hs != 0);
        const uint8_t *ref = high_speed ? s_mtp_ref_hs : s_mtp_ref_fs;
        uint8_t got[MTP_FN_DESC_LEN];
        size_t len = 0;

        esp_err_t err = mtp_build_desc(high_speed, &fake, got, sizeof(got), &len);
        if (err != ESP_OK || len != (size_t)TUD_MTP_DESC_LEN) {
            ESP_LOGE(TAG, "self-check %s: build_desc returned %s, len=%u (expected %u)",
                     high_speed ? "HS" : "FS", esp_err_to_name(err), (unsigned)len,
                     (unsigned)TUD_MTP_DESC_LEN);
            ok = false;
            continue;
        }
        bool same = true;
        for (size_t i = 0; i < len; i++) {
            if (got[i] != ref[i]) {
                ESP_LOGE(TAG, "self-check %s: byte %u differs: got 0x%02X, TUD_MTP_DESCRIPTOR says 0x%02X",
                         high_speed ? "HS" : "FS", (unsigned)i, got[i], ref[i]);
                same = false;
            }
        }
        if (same) {
            ESP_LOGI(TAG, "descriptor self-check %s: %u bytes identical to TUD_MTP_DESCRIPTOR",
                     high_speed ? "HS" : "FS", (unsigned)len);
        } else {
            ok = false;
        }
    }
    return ok;
}

#else /* !CONFIG_TINYUSB_MTP_ENABLED */

/* 本固件没有 MTP 类：没有可自检的描述符片段，函数仍然要存在（公开头里有声明）。 */
bool mtp_backend_desc_selfcheck(void)
{
    ESP_LOGW(TAG, "desc self-check skipped: CONFIG_TINYUSB_MTP_ENABLED=n");
    return true;
}

#endif /* CONFIG_TINYUSB_MTP_ENABLED */

/* ===========================================================================
 * 4. 启动
 * ========================================================================= */

#if CONFIG_TINYUSB_MTP_ENABLED
static tinyusb_mtp_storage_handle_t s_storage = NULL;
#endif

esp_err_t mtp_backend_start(void)
{
#if !CONFIG_TINYUSB_MTP_ENABLED
    ESP_LOGW(TAG, "CONFIG_TINYUSB_MTP_ENABLED=n: this firmware has no MTP class; nothing started");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_driver_installed && s_fn_registered) {
        return ESP_OK;
    }

    /* 0) 描述符形态自检（纯计算，不需要硬件，也不需要复合层先准备好） */
    if (!mtp_backend_desc_selfcheck()) {
        ESP_LOGE(TAG, "MTP descriptor fragment does NOT match TUD_MTP_DESCRIPTOR - this is a bug in "
                      "components/mtp_backend; MTP will not work. Continuing so the rest of the "
                      "firmware still boots (probe-report §2.3 的教训)");
    }

    /* 1) 卷：幂等地确保 SD 卡已挂载。失败也继续 —— 描述符形态必须稳定 */
    if (!sd_storage_is_mounted()) {
        esp_err_t sd_err = sd_storage_init();
        if (sd_err != ESP_OK) {
            ESP_LOGE(TAG, "SD volume unavailable (%s 0x%x) at '%s': MTP will still install its driver "
                          "and register IF0, but the host will see NO storage to browse",
                     esp_err_to_name(sd_err), (unsigned)sd_err, sd_storage_mount_point());
        }
    }

    /* 2) MTP 类驱动 install —— 必须在 usb_composite_start() 之前（probe-contract §3.2） */
    if (!s_driver_installed) {
        const tinyusb_mtp_driver_config_t driver_cfg = {
            .manufacturer = CONFIG_MTP_BACKEND_MANUFACTURER,
            .model = CONFIG_MTP_BACKEND_MODEL,
            .version = CONFIG_MTP_BACKEND_VERSION,
            /* NULL = 用设备描述符里的序列号；复合层按 eFuse MAC 生成（frozen-allocation §3） */
            .serial = NULL,
            .friendly_name = CONFIG_MTP_BACKEND_STORAGE_NAME,
        };
        esp_err_t err = tinyusb_mtp_install_driver(&driver_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_mtp_install_driver() failed: %s (0x%x)",
                     esp_err_to_name(err), (unsigned)err);
            return err;
        }
        s_driver_installed = true;
        ESP_LOGI(TAG, "tinyusb MTP class driver installed: manufacturer='%s' model='%s' version='%s' "
                      "friendly_name='%s' serial=<device descriptor / eFuse MAC>",
                 driver_cfg.manufacturer, driver_cfg.model, driver_cfg.version, driver_cfg.friendly_name);
    }

    /* 3) 注册**唯一**卷：base_path 直接取 sd_storage 的挂载点 —— 全工程只有那一处路径定义 */
    if (!s_storage_registered && sd_storage_is_mounted()) {
        const tinyusb_mtp_storage_config_t storage_cfg = {
            .base_path = sd_storage_mount_point(),
            .display_name = CONFIG_MTP_BACKEND_STORAGE_NAME,
            .removable = true,
        };
        esp_err_t err = tinyusb_mtp_register_storage(&storage_cfg, &s_storage);
        if (err == ESP_OK) {
            s_storage_registered = true;
            ESP_LOGI(TAG, "MTP storage registered: base_path='%s' display_name='%s' removable=1 "
                          "(唯一卷：CONFIG_TINYUSB_MTP_MAX_STORAGES=%d)",
                     storage_cfg.base_path, storage_cfg.display_name,
                     (int)CONFIG_TINYUSB_MTP_MAX_STORAGES);
        } else {
            ESP_LOGE(TAG, "tinyusb_mtp_register_storage('%s') failed: %s (0x%x) - volume stays mounted "
                          "but MTP has no storage",
                     storage_cfg.base_path, esp_err_to_name(err), (unsigned)err);
        }
    }

    /* 4) 向 I1 复合层注册功能。注册顺序决定接口号 ⇒ MTP 必须**第一个**注册（拿到 IF0） */
    if (!s_fn_registered) {
        esp_err_t err = usb_composite_register(&s_mtp_fn);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_composite_register(mtp) failed: %s (0x%x) - "
                          "mtp_backend_start() 必须在 usb_composite_prepare()/start() 之前、"
                          "且是第一个注册的功能",
                     esp_err_to_name(err), (unsigned)err);
            return err;
        }
        s_fn_registered = true;
        ESP_LOGI(TAG, "MTP function registered with usb_composite: itf_count=1, "
                      "1 interrupt IN + 1 bulk IN/OUT (FS 64 / HS 512), IAD=no, WCID=no, "
                      "on_out=<owned by the esp_tinyusb MTP class>");
    }

    return ESP_OK;
#endif /* CONFIG_TINYUSB_MTP_ENABLED */
}

bool mtp_backend_is_driver_installed(void)
{
    return s_driver_installed;
}

bool mtp_backend_is_storage_registered(void)
{
    return s_storage_registered;
}

const char *mtp_backend_mount_point(void)
{
    /* 与 FATFS 挂载点**同源**：mtp_backend 自己没有第二份路径配置 */
    return sd_storage_mount_point();
}

const char *mtp_backend_storage_name(void)
{
    return CONFIG_MTP_BACKEND_STORAGE_NAME;
}
