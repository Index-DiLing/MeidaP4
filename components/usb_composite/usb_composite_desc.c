// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I1 复合层 —— **纯计算**部分：功能注册、接口/端点/字符串分配、
// FS/HS 配置描述符 + device + qualifier + BOS + MS OS 2.0 组装、DWC2 FIFO 预算、逐字节自检。
//
// ⚠️ 本文件**不得**包含任何 ESP-IDF 运行时依赖（无 esp_log / FreeRTOS / tinyusb / NVS）。
//    它只用 <string.h> <stdio.h> + esp_err_t（宿主侧由 test/host/shim/esp_err.h 提供），
//    这样 test/host/test_composite_desc.c 能用**原生 gcc** 编出同一份代码做离线校验。
//    需要日志就把原因写进 uc_last_error()，由 usb_composite.c 打出来。
//
// 数字来源：docs/contracts/frozen-allocation.md §1/§2/§3/§4/§5/§6（冻结，不得单方面改）。
// 生产者：Lead。

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "usb_composite_internal.h"

/* 宿主侧（test/host）不经过 Kconfig，给它冻结的默认值，保证两侧形态一致：
 * 函数子集头 = 开（真机验证过的 final-b 形态）、配置子集头 = 关（见 frozen-allocation §5.2）。 */
#ifndef CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET
#define CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET 0
#endif
#ifndef CONFIG_USB_COMPOSITE_MSOS_FUNCTION_SUBSET
#define CONFIG_USB_COMPOSITE_MSOS_FUNCTION_SUBSET 1
#endif

/* =====================================================================
 * 0. 描述符类型与协议常量（值取自 USB 2.0 / MS OS 2.0 / TinyUSB usbd.h 的宏展开）
 * ===================================================================== */

#define DT_DEVICE             0x01
#define DT_CONFIG             0x02
#define DT_STRING             0x03
#define DT_INTERFACE          0x04
#define DT_ENDPOINT           0x05
#define DT_DEVICE_QUALIFIER   0x06
#define DT_IAD                0x0B
#define DT_BOS                0x0F
#define DT_DEVICE_CAPABILITY  0x10

#define CAP_PLATFORM          0x05

#define LEN_DEVICE_DESC       18
#define LEN_QUALIFIER_DESC    10
#define LEN_CONFIG_DESC        9
#define LEN_ITF_DESC           9
#define LEN_EP_DESC            7
#define LEN_IAD_DESC           8

#define LEN_BOS               5
#define LEN_CAP_WEBUSB        24
#define LEN_CAP_MSOS          28
#define LEN_BOS_TOTAL         (LEN_BOS + LEN_CAP_WEBUSB + LEN_CAP_MSOS)   /* 57 */

#define WEBUSB_VENDOR_CODE    0x01
#define MSOS_VENDOR_CODE      0x02
#define WEBUSB_LANDING_PAGE   0x00   /* 必须 0：见 frozen-allocation §5 */

#define EP_XFER_BULK_ATTR     0x02
#define EP_XFER_INTERRUPT_ATTR 0x03

/* WebUSB 平台能力 UUID {3408b638-09a9-47a0-8bfd-a0768815b665}（= TUD_BOS_WEBUSB_UUID） */
static const uint8_t k_webusb_uuid[16] = {
    0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
    0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65
};
/* MS OS 2.0 平台能力 UUID {d8dd60df-4589-4cc7-9cd2-659d9e648a9f}（= TUD_BOS_MS_OS_20_UUID） */
static const uint8_t k_msos_uuid[16] = {
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F
};

/* MS OS 2.0 描述符集（值与 tusb_types.h 的枚举一致） */
#define MSOS_SET_HEADER          0x0000
#define MSOS_SUBSET_HEADER_CONFIG 0x0001
#define MSOS_SUBSET_HEADER_FUNC  0x0002
#define MSOS_FEATURE_COMPAT_ID   0x0003
#define MSOS_FEATURE_REG_PROP    0x0004
#define MSOS_WINDOWS_VERSION     0x06030000u

#define MSOS_SET_HEADER_LEN      0x000A
#define MSOS_SUBSET_HEADER_LEN   0x0008
#define MSOS_COMPAT_ID_LEN       0x0014
#define MSOS_REGPROP_NAME_LEN    40    /* ("DeviceInterfaceGUID" + NUL) × 2 = (19+1)*2 */
#define MSOS_REGPROP_DATA_LEN    78    /* ("{A5DCBF10-…}" + NUL) × 2 = (38+1)*2 */
#define MSOS_REGPROP_LEN         (10 + MSOS_REGPROP_NAME_LEN + MSOS_REGPROP_DATA_LEN)  /* 128 */
#define MSOS_SUBSET_LEN          (MSOS_SUBSET_HEADER_LEN + MSOS_COMPAT_ID_LEN + MSOS_REGPROP_LEN)  /* 156 */

/* 冻结的默认值 */
#define DEF_VID               0x303A
#define DEF_PID               0x4103   /* ⚠️ 刻意在 esp_tinyusb 自动 PID 空间 0x4000~0x407F 之外 */
#define DEF_BCD_USB           0x0210
#define DEF_BCD_DEVICE        0x0100
#define DEF_MANUFACTURER      "MeidaP4"
#define DEF_PRODUCT           "MeidaP4 Composite"
#define DEF_SERIAL            "COMPOSITE-0001"
#define DEF_WCID_COMPAT_ID    "WINUSB"
#define GUID_DEVINTERFACE_USB_DEVICE "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"

/* HS 口 DWC2 硬件上限（dwc2_esp32.h：HS .ep_count = 16 / .ep_in_count = 8 / dfifo 1024 word） */
#define HS_EP_COUNT           16
#define HS_EP_IN_COUNT        8     /* 含 EP0 → 用户 IN ≤ 7 */
#define HS_DFIFO_WORDS        USB_COMPOSITE_FIFO_WORDS_TOTAL
#define EP0_MPS               64

/* =====================================================================
 * 1. 状态（全部 static：无堆分配，便于宿主测试）
 * ===================================================================== */

typedef struct {
    const usb_function_t *fn[USB_COMPOSITE_MAX_FUNCTIONS];
    usb_fn_alloc_t alloc[USB_COMPOSITE_MAX_FUNCTIONS];
    uint8_t fn_count;

    usb_composite_identity_t id;
    bool id_set;

    const char *str[USB_COMPOSITE_MAX_STR];
    int str_count;

    uint8_t dev[LEN_DEVICE_DESC];
    uint8_t qual[LEN_QUALIFIER_DESC];
    uint8_t cfg[2][USB_COMPOSITE_CFG_DESC_MAX];   /* [0]=FS [1]=HS */
    uint32_t cfg_len[2];
    uint8_t bos[USB_COMPOSITE_BOS_MAX];
    uint32_t bos_len;
    uint8_t msos[USB_COMPOSITE_MSOS_MAX];
    uint32_t msos_len;

    usb_composite_budget_t budget;
    bool prepared;

    char err[224];
} uc_state_t;

static uc_state_t s;
static const char s_langid[2] = { 0x09, 0x04 };

static void uc_fail(const char *fmt, ...)
{
    /* 纯 C：手写可变参数（避免 stdarg 之外的花样） */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.err, sizeof(s.err), fmt, ap);
    va_end(ap);
}

/* =====================================================================
 * 2. 小工具
 * ===================================================================== */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
/* 自检用的读取器（名字带 _local 是为了不和 wu_codec.h 里的 wu_rd16/wu_rd32 混） */
static uint16_t wu_rd16_local(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t wu_rd_local32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** ASCII -> UTF-16LE，返回写入的字节数（含终止 NUL 的两个字节） */
static size_t ascii_to_utf16le(uint8_t *dst, size_t cap, const char *ascii)
{
    size_t n = strlen(ascii);
    if ((n + 1) * 2 > cap) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        dst[i * 2] = (uint8_t)ascii[i];
        dst[i * 2 + 1] = 0x00;
    }
    dst[n * 2] = 0x00;
    dst[n * 2 + 1] = 0x00;
    return (n + 1) * 2;
}

static int wc(const usb_composite_identity_t *id) { return (int)id->max_power_ma; }

/* =====================================================================
 * 3. 注册
 * ===================================================================== */

esp_err_t uc_register(const usb_function_t *fn)
{
    if (s.prepared) {
        uc_fail("uc_register: 已经 prepare 过，注册必须在 prepare 之前完成（frozen-allocation §2.1）");
        return ESP_ERR_INVALID_STATE;
    }
    if (fn == NULL || fn->build_desc == NULL || fn->itf_count == 0) {
        uc_fail("uc_register: fn/build_desc/itf_count 非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (s.fn_count >= USB_COMPOSITE_MAX_FUNCTIONS) {
        uc_fail("uc_register: 功能数超过上限 %d", USB_COMPOSITE_MAX_FUNCTIONS);
        return ESP_ERR_NO_MEM;
    }
    if (fn->ep_in_count > USB_COMPOSITE_MAX_EP_PER_DIR ||
        fn->ep_out_count > USB_COMPOSITE_MAX_EP_PER_DIR) {
        uc_fail("uc_register(%s): 单方向端点数超过上限 %d", fn->name ? fn->name : "?",
                USB_COMPOSITE_MAX_EP_PER_DIR);
        return ESP_ERR_INVALID_ARG;
    }
    if (fn->ep_out_count > fn->ep_in_count) {
        /* 每个 OUT 要么配对某个 IN、要么独立分配；OUT 比 IN 多没有意义且容易写错 */
        uc_fail("uc_register(%s): ep_out_count(%u) > ep_in_count(%u)",
                fn->name ? fn->name : "?", fn->ep_out_count, fn->ep_in_count);
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t k = 0; k < fn->ep_out_count; k++) {
        uint8_t src = fn->ep_out_same_num_as_in[k];
        if (src != USB_EP_NO_PAIR && src >= fn->ep_in_count) {
            uc_fail("uc_register(%s): ep_out_same_num_as_in[%u]=%u 越界（ep_in_count=%u）",
                    fn->name ? fn->name : "?", k, src, fn->ep_in_count);
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (uint8_t k = 0; k < fn->ep_in_count; k++) {
        uint8_t a = (uint8_t)fn->ep_in_req[k].xfer;
        if (a != USB_EP_XFER_BULK && a != USB_EP_XFER_INTERRUPT) {
            uc_fail("uc_register(%s): ep_in_req[%u].xfer 非法", fn->name ? fn->name : "?", k);
            return ESP_ERR_INVALID_ARG;
        }
        if (fn->ep_in_req[k].mps_hs == 0 || fn->ep_in_req[k].mps_fs == 0) {
            uc_fail("uc_register(%s): ep_in_req[%u] 的 mps 不能为 0", fn->name ? fn->name : "?", k);
            return ESP_ERR_INVALID_ARG;
        }
        if (fn->ep_in_req[k].xfer == USB_EP_XFER_BULK &&
            (fn->ep_in_req[k].mps_hs > 512 || fn->ep_in_req[k].mps_fs > 64)) {
            /* USB 2.0：HS bulk ≤ 512，FS bulk ≤ 64 */
            uc_fail("uc_register(%s): ep_in_req[%u] bulk mps 超限（HS %u / FS %u）",
                    fn->name ? fn->name : "?", k,
                    fn->ep_in_req[k].mps_hs, fn->ep_in_req[k].mps_fs);
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (uint8_t k = 0; k < fn->ep_out_count; k++) {
        if (fn->ep_out_req[k].mps_hs == 0 || fn->ep_out_req[k].mps_fs == 0) {
            uc_fail("uc_register(%s): ep_out_req[%u] 的 mps 不能为 0", fn->name ? fn->name : "?", k);
            return ESP_ERR_INVALID_ARG;
        }
        if (fn->ep_out_req[k].xfer == USB_EP_XFER_BULK &&
            (fn->ep_out_req[k].mps_hs > 512 || fn->ep_out_req[k].mps_fs > 64)) {
            uc_fail("uc_register(%s): ep_out_req[%u] bulk mps 超限", fn->name ? fn->name : "?", k);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (fn->wcid && (fn->wcid_compatible_id == NULL || fn->wcid_device_interface_guid == NULL)) {
        uc_fail("uc_register(%s): wcid=true 但没有给 compatible_id / device_interface_guid",
                fn->name ? fn->name : "?");
        return ESP_ERR_INVALID_ARG;
    }

    /* ⚠️ **必须复制结构体，不能保存调用方指针**（F9，verifier 2026-10 集成测试实测）：
     *
     *   `usb_net_ncm.c` 曾经在**栈上**构造 `usb_function_t` 后注册 → 函数返回即悬垂 →
     *   `uc_prepare()` 在**另一层调用栈**上解引用它，读到被栈复用覆写的内存：
     *     · 轻则 `ep_in_count` 变垃圾 → 报"IN 端点号耗尽（HS 上限 16）"（4 功能 6 端点明明够用）
     *     · 重则 `build_desc` **函数指针**是垃圾 → 跳转到随机地址（比报错更糟）
     *   注意 `name` 在结构体偏移 0，所以日志里还认得是 "NCM" —— 这是典型悬垂栈内存指纹。
     *
     *   对照：MTP（`static`）与 vendor video/ctrl（全局 `const`）都安全，只有 NCM 踩中。
     *   复制之后调用方可以安全地传**栈上**对象；但**回调与字符串指针仍必须常驻**
     *   （它们被原样存进副本，不做深拷贝）—— 这一点已写进 `usb_composite.h`。 */
    static usb_function_t s_fn_copy[USB_COMPOSITE_MAX_FUNCTIONS];
    s_fn_copy[s.fn_count] = *fn;
    s.fn[s.fn_count] = &s_fn_copy[s.fn_count];
    s.fn_count++;
    return ESP_OK;
}

esp_err_t uc_set_identity(const usb_composite_identity_t *id)
{
    if (s.prepared) {
        uc_fail("uc_set_identity: 已经 prepare 过");
        return ESP_ERR_INVALID_STATE;
    }
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s.id = *id;
    s.id_set = true;
    return ESP_OK;
}

bool uc_is_prepared(void) { return s.prepared; }
const char *uc_last_error(void) { return s.err; }

/* =====================================================================
 * 4. 字符串索引分配（frozen-allocation §4 的确定性规则）
 * ===================================================================== */

static int intern_string(const char *str)
{
    if (str == NULL || str[0] == '\0') {
        return 0;
    }
    for (int i = 4; i < s.str_count; i++) {   /* 0..3 是固定的，只在前 4 之后去重 */
        if (s.str[i] != NULL && strcmp(s.str[i], str) == 0) {
            return i;
        }
    }
    if (s.str_count >= USB_COMPOSITE_MAX_STR) {
        return -1;
    }
    s.str[s.str_count] = str;
    return s.str_count++;
}

/* =====================================================================
 * 5. 端点分配
 * ===================================================================== */

static esp_err_t alloc_endpoints(void)
{
    bool in_used[HS_EP_COUNT] = { false };
    bool out_used[HS_EP_COUNT] = { false };
    in_used[0] = true;  /* EP0 */
    out_used[0] = true;

    int itf_next = 0;
    int epin_next = 1;
    int epout_next = 1;
    int vendor_next = 0;

    for (uint8_t i = 0; i < s.fn_count; i++) {
        const usb_function_t *fn = s.fn[i];
        usb_fn_alloc_t *a = &s.alloc[i];
        memset(a, 0, sizeof(*a));

        /* --- 接口号 --- */
        if (itf_next + fn->itf_count > USB_COMPOSITE_MAX_ITF) {
            uc_fail("接口号预算超限：已用 %d + 本功能 %u > %d",
                    itf_next, fn->itf_count, USB_COMPOSITE_MAX_ITF);
            return ESP_ERR_NOT_SUPPORTED;
        }
        a->itf_count = fn->itf_count;
        for (uint8_t k = 0; k < fn->itf_count; k++) {
            a->itf[k] = (uint8_t)itf_next++;
        }

        /* --- IN 端点 --- */
        a->ep_in_count = fn->ep_in_count;
        for (uint8_t k = 0; k < fn->ep_in_count; k++) {
            while (epin_next < HS_EP_COUNT && in_used[epin_next]) {
                epin_next++;
            }
            if (epin_next >= HS_EP_COUNT) {
                uc_fail("%s: IN 端点号耗尽（HS 上限 %d，含 EP0 用户可用 %d）",
                        fn->name ? fn->name : "?", HS_EP_COUNT, HS_EP_IN_COUNT - 1);
                return ESP_ERR_NOT_SUPPORTED;
            }
            in_used[epin_next] = true;
            a->ep_in[k] = (uint8_t)(0x80 | epin_next);
            epin_next++;
        }

        /* --- OUT 端点（显式配对表） --- */
        a->ep_out_count = fn->ep_out_count;
        for (uint8_t k = 0; k < fn->ep_out_count; k++) {
            uint8_t src = fn->ep_out_same_num_as_in[k];
            uint8_t num;
            if (src != USB_EP_NO_PAIR) {
                num = (uint8_t)(a->ep_in[src] & 0x7F);
            } else {
                while (epout_next < HS_EP_COUNT && out_used[epout_next]) {
                    epout_next++;
                }
                if (epout_next >= HS_EP_COUNT) {
                    uc_fail("%s: OUT 端点号耗尽", fn->name ? fn->name : "?");
                    return ESP_ERR_NOT_SUPPORTED;
                }
                num = (uint8_t)epout_next++;
            }
            if (out_used[num]) {
                uc_fail("%s: OUT 端点号 EP%u 重复占用（配对表见 usb_composite.h）",
                        fn->name ? fn->name : "?", num);
                return ESP_ERR_INVALID_STATE;
            }
            out_used[num] = true;
            a->ep_out[k] = num;
        }

        /* --- 字符串（接口串；extra_string 在第二遍） --- */
        if (fn->itf_string != NULL) {
            int idx = intern_string(fn->itf_string);
            if (idx < 0) {
                uc_fail("%s: 字符串描述符超上限 %d（见 frozen-allocation §4）",
                        fn->name ? fn->name : "?", USB_COMPOSITE_MAX_STR);
                return ESP_ERR_NOT_SUPPORTED;
            }
            a->itf_str_idx = (uint8_t)idx;
        }

        a->vendor_idx = fn->is_vendor ? (uint8_t)vendor_next++ : USB_FN_NO_VENDOR_IDX;
    }

    /* 第二遍：extra_string（NCM 的 iMACAddress）——必须在所有接口串之后，见 §4 */
    for (uint8_t i = 0; i < s.fn_count; i++) {
        const usb_function_t *fn = s.fn[i];
        if (fn->extra_string == NULL) {
            continue;
        }
        int idx = intern_string(fn->extra_string);
        if (idx < 0) {
            uc_fail("%s: extra_string 超上限 %d", fn->name ? fn->name : "?", USB_COMPOSITE_MAX_STR);
            return ESP_ERR_NOT_SUPPORTED;
        }
        s.alloc[i].extra_str_idx = (uint8_t)idx;
    }

    return ESP_OK;
}

/* =====================================================================
 * 6. 配置描述符组装
 * ===================================================================== */

static esp_err_t validate_fragment(const usb_function_t *fn, const usb_fn_alloc_t *a,
                                   const uint8_t *frag, size_t len)
{
    size_t off = 0;
    int itf_seen = 0;
    size_t ep_seen = 0;
    int next_itf = a->itf[0];

    while (off + 2 <= len) {
        uint8_t dlen = frag[off];
        uint8_t dtype = frag[off + 1];
        if (dlen < 2 || off + dlen > len) {
            uc_fail("%s: 描述符片段在偏移 %u 处 bLength=%u 越界（片段总长 %u）",
                    fn->name ? fn->name : "?", (unsigned)off, dlen, (unsigned)len);
            return ESP_ERR_INVALID_STATE;
        }
        if (dtype == DT_INTERFACE) {
            if (dlen != LEN_ITF_DESC) {
                uc_fail("%s: 接口描述符 bLength=%u（应为 9）", fn->name ? fn->name : "?", dlen);
                return ESP_ERR_INVALID_STATE;
            }
            const uint8_t itf_num = frag[off + 2];
            const uint8_t alt = frag[off + 3];
            if (alt == 0) {
                /* 一个新接口的主设置 */
                if (itf_num != (uint8_t)next_itf) {
                    uc_fail("%s: 接口号不连续：片段里是 %u，应为 %u（分配表 itf[%d]=%u）",
                            fn->name ? fn->name : "?", itf_num, next_itf, itf_seen, next_itf);
                    return ESP_ERR_INVALID_STATE;
                }
                if (itf_seen >= (int)a->itf_count) {
                    uc_fail("%s: 片段里的接口数超过 itf_count=%u",
                            fn->name ? fn->name : "?", a->itf_count);
                    return ESP_ERR_INVALID_STATE;
                }
                next_itf++;
                itf_seen++;
            } else {
                /* ⚠️ 备用设置（alternate setting）：**同一个接口号的另一套端点配置**，
                 * 不是新接口。CDC-NCM 数据接口**必须**有 alt=0(0 端点) / alt=1(2 端点) 两套
                 * —— Windows 收件箱 UsbNcm.sys 就是这么要求的（WS-net 2026-10 提出）。
                 * 所以这里只要求它属于"上一个"接口，**不计入接口数**。 */
                if (itf_seen == 0 || itf_num != (uint8_t)(next_itf - 1)) {
                    uc_fail("%s: 备用设置 bAlternateSetting=%u 的接口号 %u 不匹配上一个接口 %u"
                            "（alt>0 必须复用前一个接口号）",
                            fn->name ? fn->name : "?", alt, itf_num,
                            itf_seen > 0 ? (unsigned)(next_itf - 1) : 0u);
                    return ESP_ERR_INVALID_STATE;
                }
            }
        } else if (dtype == DT_ENDPOINT) {
            if (dlen != LEN_EP_DESC) {
                uc_fail("%s: 端点描述符 bLength=%u（应为 7）", fn->name ? fn->name : "?", dlen);
                return ESP_ERR_INVALID_STATE;
            }
            uint8_t addr = frag[off + 2];
            bool found = false;
            if (addr & 0x80) {
                for (uint8_t k = 0; k < a->ep_in_count; k++) {
                    if (a->ep_in[k] == addr) { found = true; break; }
                }
            } else {
                for (uint8_t k = 0; k < a->ep_out_count; k++) {
                    if (a->ep_out[k] == addr) { found = true; break; }
                }
            }
            if (!found) {
                uc_fail("%s: 片段用了未分配的端点 0x%02X（IN 分配见 usb_composite.h 的 ep_in[]）",
                        fn->name ? fn->name : "?", addr);
                return ESP_ERR_INVALID_STATE;
            }
            ep_seen++;
        }
        off += dlen;
    }
    if (off != len) {
        uc_fail("%s: 描述符片段长度不自洽（解析到 %u，实际 %u）",
                fn->name ? fn->name : "?", (unsigned)off, (unsigned)len);
        return ESP_ERR_INVALID_STATE;
    }
    if (itf_seen != (int)a->itf_count) {
        uc_fail("%s: 片段里的接口数 %d 与注册的 itf_count %u 不符",
                fn->name ? fn->name : "?", itf_seen, a->itf_count);
        return ESP_ERR_INVALID_STATE;
    }
    (void)ep_seen;
    return ESP_OK;
}

static esp_err_t build_cfg(bool hs, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (cap < LEN_CONFIG_DESC) {
        uc_fail("配置描述符缓冲太小");
        return ESP_ERR_INVALID_SIZE;
    }

    size_t off = LEN_CONFIG_DESC;   /* 头部先留空，最后回填（wTotalLength 要等总长确定） */
    int itf_total = 0;

    for (uint8_t i = 0; i < s.fn_count; i++) {
        const usb_function_t *fn = s.fn[i];
        const usb_fn_alloc_t *a = &s.alloc[i];

        size_t iad_len = fn->use_iad ? LEN_IAD_DESC : 0;
        if (off + iad_len >= cap) {
            uc_fail("%s: 配置描述符缓冲不足（需要 >%u）", fn->name ? fn->name : "?", (unsigned)(off + iad_len));
            return ESP_ERR_INVALID_SIZE;
        }

        size_t frag_len = 0;
        esp_err_t err = fn->build_desc(hs, a, buf + off + iad_len, cap - off - iad_len, &frag_len);
        if (err != ESP_OK) {
            uc_fail("%s: build_desc(%s) 返回 %d", fn->name ? fn->name : "?", hs ? "HS" : "FS", (int)err);
            return err;
        }
        if (frag_len == 0) {
            uc_fail("%s: build_desc 产出 0 字节", fn->name ? fn->name : "?");
            return ESP_ERR_INVALID_STATE;
        }
        err = validate_fragment(fn, a, buf + off + iad_len, frag_len);
        if (err != ESP_OK) {
            return err;
        }

        if (fn->use_iad) {
            /* IAD 的类字段**抄自片段里第一个接口描述符**（USB 规范要求二者一致），
             * 这样功能不需要再多声明一组字段，也不会写错。 */
            const uint8_t *itf0 = buf + off + iad_len;
            uint8_t *d = buf + off;
            d[0] = LEN_IAD_DESC;
            d[1] = DT_IAD;
            d[2] = a->itf[0];
            d[3] = a->itf_count;
            d[4] = itf0[5];
            d[5] = itf0[6];
            d[6] = itf0[7];
            d[7] = 0;
        }

        off += iad_len + frag_len;
        itf_total += a->itf_count;
    }

    if (off > 0xFFFF) {
        uc_fail("配置描述符超过 65535 字节");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t attr = 0x80;                        /* bit7 必须为 1（USB 2.0 §9.6.3） */
    if (s.id.self_powered) {
        attr |= 0x40;
    }
    uint16_t power = (uint16_t)((wc(&s.id) + 1) / 2);   /* 单位 2 mA，向上取整 */

    buf[0] = LEN_CONFIG_DESC;
    buf[1] = DT_CONFIG;
    put_u16(buf + 2, (uint16_t)off);
    buf[4] = (uint8_t)itf_total;
    buf[5] = 0x01;                              /* bConfigurationValue */
    buf[6] = 0x00;                              /* iConfiguration */
    buf[7] = attr;
    buf[8] = (uint8_t)power;

    *out_len = off;
    return ESP_OK;
}

/* =====================================================================
 * 7. device / qualifier / BOS / MS OS 2.0
 * ===================================================================== */

static void build_device_desc(void)
{
    uint8_t *d = s.dev;
    d[0] = LEN_DEVICE_DESC;
    d[1] = DT_DEVICE;
    put_u16(d + 2, s.id.bcd_usb);
    d[4] = s.id.b_device_class;
    d[5] = s.id.b_device_subclass;
    d[6] = s.id.b_device_protocol;
    d[7] = EP0_MPS;
    put_u16(d + 8, s.id.vid);
    put_u16(d + 10, s.id.pid);
    put_u16(d + 12, s.id.bcd_device);
    d[14] = 1;   /* iManufacturer */
    d[15] = 2;   /* iProduct */
    d[16] = 3;   /* iSerialNumber */
    d[17] = 1;   /* bNumConfigurations */
}

static void build_qualifier_desc(void)
{
    uint8_t *d = s.qual;
    d[0] = LEN_QUALIFIER_DESC;
    d[1] = DT_DEVICE_QUALIFIER;
    put_u16(d + 2, s.id.bcd_usb);
    d[4] = s.id.b_device_class;
    d[5] = s.id.b_device_subclass;
    d[6] = s.id.b_device_protocol;
    d[7] = EP0_MPS;
    d[8] = 1;   /* bNumConfigurations */
    d[9] = 0;   /* bReserved */
}

static void build_bos(uint16_t msos_set_len)
{
    uint8_t *d = s.bos;
    memset(d, 0, sizeof(s.bos));

    d[0] = LEN_BOS;
    d[1] = DT_BOS;
    put_u16(d + 2, LEN_BOS_TOTAL);
    d[4] = 2;

    /* WebUSB 平台能力 */
    uint8_t *w = d + LEN_BOS;
    w[0] = LEN_CAP_WEBUSB;
    w[1] = DT_DEVICE_CAPABILITY;
    w[2] = CAP_PLATFORM;
    w[3] = 0x00;
    memcpy(w + 4, k_webusb_uuid, 16);
    put_u16(w + 20, 0x0100);          /* bcdVersion */
    w[22] = WEBUSB_VENDOR_CODE;
    w[23] = WEBUSB_LANDING_PAGE;      /* 必须 0 */

    /* MS OS 2.0 平台能力 */
    uint8_t *m = d + LEN_BOS + LEN_CAP_WEBUSB;
    m[0] = LEN_CAP_MSOS;
    m[1] = DT_DEVICE_CAPABILITY;
    m[2] = CAP_PLATFORM;
    m[3] = 0x00;
    memcpy(m + 4, k_msos_uuid, 16);
    put_u32(m + 20, MSOS_WINDOWS_VERSION);
    put_u16(m + 24, msos_set_len);
    m[26] = MSOS_VENDOR_CODE;
    m[27] = 0;

    s.bos_len = LEN_BOS_TOTAL;
}

/**
 * 组装 MS OS 2.0 描述符集。
 *
 * 形态（frozen-allocation §5）：Set Header → [可选 Config Subset] → 每个 WCID 功能
 * Function Subset Header → CompatibleID → Registry Property(单数 DeviceInterfaceGUID + REG_SZ)。
 * 不开 Config Subset 时，单个 WCID 功能 = 10 + 8 + 20 + 128 = **166 字节**，
 * 与 probe 阶段真机验证过的 final-b 形态逐字节一致。
 */
static esp_err_t build_msos(void)
{
    uint8_t *d = s.msos;
    size_t off = 0;

    int wcid_n = 0;
    for (uint8_t i = 0; i < s.fn_count; i++) {
        if (s.fn[i]->wcid) {
            wcid_n++;
        }
    }
    if (wcid_n == 0) {
        uc_fail("MS OS 2.0: 没有任何 wcid=true 的功能（vendor 视频/控制接口都需要 WinUSB）");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 每个 WCID 功能一个 Function Subset（8 + 20 + 128 = 156 B）。
     * 2026-10-22 裁决（T2）：**IF1 视频接口也要 WCID**。
     * 原因（真机实测）：Windows 只给 MS OS Function Subset 里列出的接口装 winusb.sys；
     * MI_01 没有驱动 ⇒ 没有 device interface ⇒ Chrome（以及主机 WinUSB 工具）**打不开 EP3**，
     * 视频通道在"枚举成功"的假象下永远收不到数据。所以 2 个 vendor 接口各发一个 subset。
     * 总长 = 10 + 2×156 = 322 B（≤ USB_COMPOSITE_MSOS_MAX 512），仍不开 Configuration Subset
     * （bNumConfigurations=1，不需要它消歧）。
     */
    size_t total = MSOS_SET_HEADER_LEN + (size_t)wcid_n * MSOS_SUBSET_LEN;
#if CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET
    total += MSOS_SUBSET_HEADER_LEN;
#endif
    if (total > sizeof(s.msos)) {
        uc_fail("MS OS 2.0 描述符集 %u 字节超过缓冲 %u", (unsigned)total, (unsigned)sizeof(s.msos));
        return ESP_ERR_INVALID_SIZE;
    }

    /* Set Header */
    put_u16(d + 0, MSOS_SET_HEADER_LEN);
    put_u16(d + 2, MSOS_SET_HEADER);
    put_u32(d + 4, MSOS_WINDOWS_VERSION);
    put_u16(d + 8, (uint16_t)total);
    off = MSOS_SET_HEADER_LEN;

#if CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET
    /* Configuration Subset Header（默认关；开了就与真机验证过的形态不一致） */
    put_u16(d + off + 0, MSOS_SUBSET_HEADER_LEN);
    put_u16(d + off + 2, MSOS_SUBSET_HEADER_CONFIG);
    d[off + 4] = 0x01;                                    /* bConfigurationValue */
    d[off + 5] = 0x00;
    put_u16(d + off + 6, (uint16_t)(total - MSOS_SET_HEADER_LEN));
    off += MSOS_SUBSET_HEADER_LEN;
#endif

    /* 逐个 WCID 功能：Function Subset Header → CompatibleID → Registry Property */
    for (uint8_t i = 0; i < s.fn_count; i++) {
        const usb_function_t *fn = s.fn[i];
        const usb_fn_alloc_t *a = &s.alloc[i];

        if (!fn->wcid) {
            continue;
        }

        /* Function Subset Header（bFirstInterfaceNumber = 该 WCID 功能的首接口） */
        put_u16(d + off + 0, MSOS_SUBSET_HEADER_LEN);
        put_u16(d + off + 2, MSOS_SUBSET_HEADER_FUNC);
        d[off + 4] = a->itf[0];
        d[off + 5] = 0x00;
        put_u16(d + off + 6, (uint16_t)MSOS_SUBSET_LEN);
        off += MSOS_SUBSET_HEADER_LEN;

        /* Compatible ID：CompatibleID = "WINUSB\0\0"（补零到 8 字节），SubCompatibleID = 8×0x00 */
        put_u16(d + off + 0, MSOS_COMPAT_ID_LEN);
        put_u16(d + off + 2, MSOS_FEATURE_COMPAT_ID);
        memset(d + off + 4, 0, 16);
        {
            const char *cid = (fn->wcid_compatible_id != NULL) ? fn->wcid_compatible_id
                                                               : DEF_WCID_COMPAT_ID;
            size_t n = strlen(cid);
            if (n > 8) {
                uc_fail("MS OS 2.0: compatible_id \"%s\" 超过 8 字节", cid);
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(d + off + 4, cid, n);
        }
        off += MSOS_COMPAT_ID_LEN;

        /* Registry Property：**单数 DeviceInterfaceGUID + REG_SZ(0x0001)**（真机唯一可用形态） */
        {
            uint8_t *rp = d + off;
            const char *guid = (fn->wcid_device_interface_guid != NULL) ? fn->wcid_device_interface_guid
                                                                        : GUID_DEVINTERFACE_USB_DEVICE;
            if (strlen(guid) != 38) {
                uc_fail("MS OS 2.0: device_interface_guid 必须是 38 字符的 {…} 文本，实际 %u 字符（\"%s\"）",
                        (unsigned)strlen(guid), guid);
                return ESP_ERR_INVALID_ARG;
            }
            put_u16(rp + 0, MSOS_REGPROP_LEN);
            put_u16(rp + 2, MSOS_FEATURE_REG_PROP);
            put_u16(rp + 4, 0x0001);              /* REG_SZ —— 不是 0x0007 REG_MULTI_SZ */
            put_u16(rp + 6, MSOS_REGPROP_NAME_LEN);
            if (ascii_to_utf16le(rp + 8, MSOS_REGPROP_NAME_LEN, "DeviceInterfaceGUID")
                != MSOS_REGPROP_NAME_LEN) {
                uc_fail("MS OS 2.0: 属性名编码长度不符");
                return ESP_ERR_INVALID_STATE;
            }
            put_u16(rp + 8 + MSOS_REGPROP_NAME_LEN, MSOS_REGPROP_DATA_LEN);
            if (ascii_to_utf16le(rp + 10 + MSOS_REGPROP_NAME_LEN, MSOS_REGPROP_DATA_LEN, guid)
                != MSOS_REGPROP_DATA_LEN) {
                uc_fail("MS OS 2.0: 属性值编码长度不符");
                return ESP_ERR_INVALID_STATE;
            }
            off += MSOS_REGPROP_LEN;
        }
    }

    if (off != total) {
        uc_fail("MS OS 2.0: 组装长度 %u 与预算 %u 不一致", (unsigned)off, (unsigned)total);
        return ESP_ERR_INVALID_STATE;
    }
    s.msos_len = (uint32_t)off;
    return ESP_OK;
}

/* =====================================================================
 * 8. DWC2 FIFO 预算（frozen-allocation §2.2；plan 未算，是真正的硬约束）
 * ===================================================================== */

/** dcd_dwc2.c:calc_device_grxfsiz —— 与 IDF/TinyUSB 源码同式 */
static uint16_t grxfsiz_words(uint16_t largest_ep_size, uint8_t ep_count)
{
    return (uint16_t)(13 + 1 + 2 * ((largest_ep_size / 4) + 1) + 2 * ep_count);
}

static uint16_t ceil4u(uint16_t v) { return (uint16_t)(((v + 3u) / 4u) * 4u); }

static uint16_t calc_fifo_words(void)
{
    uint16_t max_out = EP0_MPS;
    uint16_t sum_in = 0;

    for (uint8_t i = 0; i < s.fn_count; i++) {
        const usb_function_t *fn = s.fn[i];
        for (uint8_t k = 0; k < fn->ep_in_count; k++) {
            sum_in = (uint16_t)(sum_in + (fn->ep_in_req[k].mps_hs + 3u) / 4u);
        }
        for (uint8_t k = 0; k < fn->ep_out_count; k++) {
            uint16_t m = ceil4u(fn->ep_out_req[k].mps_hs);
            if (m > max_out) {
                max_out = m;
            }
        }
    }

    /* Buffer DMA（P4 默认）要为每个端点方向留 1 word 的 DxEPDMAn：2 * ep_count */
    uint16_t used = (uint16_t)(2 * HS_EP_COUNT);
    used = (uint16_t)(used + grxfsiz_words(max_out, HS_EP_COUNT));
    used = (uint16_t)(used + (EP0_MPS + 3u) / 4u);
    used = (uint16_t)(used + sum_in);
    return used;
}

/* =====================================================================
 * 9. 组装入口
 * ===================================================================== */

esp_err_t uc_prepare(void)
{
    if (s.prepared) {
        return ESP_OK;   /* 幂等 */
    }
    s.err[0] = '\0';
    if (s.fn_count == 0) {
        uc_fail("uc_prepare: 没有注册任何功能");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.id_set) {
        usb_composite_identity_default(&s.id);
    }

    /* 字符串 0..3 固定；4.. 由 alloc_endpoints() 按注册顺序 + 内容去重分配 */
    memset(s.str, 0, sizeof(s.str));
    s.str[0] = s_langid;
    s.str[1] = (s.id.manufacturer != NULL) ? s.id.manufacturer : DEF_MANUFACTURER;
    s.str[2] = (s.id.product != NULL) ? s.id.product : DEF_PRODUCT;
    s.str[3] = (s.id.serial != NULL) ? s.id.serial : DEF_SERIAL;
    s.str_count = 4;

    esp_err_t err = alloc_endpoints();
    if (err != ESP_OK) {
        return err;
    }

    int itf_total = 0;
    int ep_in_total = 0;
    int ep_out_total = 0;
    int vendor_total = 0;
    for (uint8_t i = 0; i < s.fn_count; i++) {
        itf_total += s.alloc[i].itf_count;
        ep_in_total += s.alloc[i].ep_in_count;
        ep_out_total += s.alloc[i].ep_out_count;
        if (s.fn[i]->is_vendor) {
            vendor_total++;
        }
    }
    if (ep_in_total > HS_EP_IN_COUNT - 1) {
        uc_fail("用户 IN 端点 %d 个，超过 HS 上限 %d（含 EP0 共 8）", ep_in_total, HS_EP_IN_COUNT - 1);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (itf_total > USB_COMPOSITE_MAX_ITF) {
        uc_fail("接口总数 %d 超过上限 %d", itf_total, USB_COMPOSITE_MAX_ITF);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s.str_count > USB_COMPOSITE_MAX_STR) {
        uc_fail("字符串描述符 %d 条超过 esp_tinyusb 上限 %d", s.str_count, USB_COMPOSITE_MAX_STR);
        return ESP_ERR_NOT_SUPPORTED;
    }

    build_device_desc();
    build_qualifier_desc();

    size_t n = 0;
    err = build_cfg(false, s.cfg[0], sizeof(s.cfg[0]), &n);
    if (err != ESP_OK) {
        return err;
    }
    s.cfg_len[0] = (uint32_t)n;
    err = build_cfg(true, s.cfg[1], sizeof(s.cfg[1]), &n);
    if (err != ESP_OK) {
        return err;
    }
    s.cfg_len[1] = (uint32_t)n;

    err = build_msos();
    if (err != ESP_OK) {
        return err;
    }
    build_bos((uint16_t)s.msos_len);

    /* 预算 */
    s.budget.itf_total = (uint8_t)itf_total;
    s.budget.fn_total = s.fn_count;
    s.budget.vendor_fn_total = (uint8_t)vendor_total;
    s.budget.ep_in_total = (uint8_t)ep_in_total;
    s.budget.ep_out_total = (uint8_t)ep_out_total;
    s.budget.fifo_words_total = HS_DFIFO_WORDS;
    s.budget.fifo_words_used = calc_fifo_words();
    s.budget.fifo_words_free = (uint16_t)(HS_DFIFO_WORDS - s.budget.fifo_words_used);
    s.budget.fs_cfg_len = s.cfg_len[0];
    s.budget.hs_cfg_len = s.cfg_len[1];
    s.budget.bos_len = s.bos_len;
    s.budget.msos_len = s.msos_len;
    s.budget.str_count = (uint32_t)s.str_count;

    if (s.budget.fifo_words_used > HS_DFIFO_WORDS) {
        uc_fail("DWC2 FIFO 预算超限：需要 %u word，只有 %u（见 frozen-allocation §2.2）。"
                "注意 bm_double_buffered 必须保持 0",
                s.budget.fifo_words_used, HS_DFIFO_WORDS);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s.prepared = true;
    return ESP_OK;
}

/* =====================================================================
 * 10. 自检（逐字节解析自己组装出来的描述符）
 * ===================================================================== */

typedef struct {
    uint32_t run;
    uint32_t fail;
    bool first_done;
} chk_t;

static void chk(chk_t *c, bool ok, const char *fmt, ...)
{
    c->run++;
    if (ok) {
        return;
    }
    c->fail++;
    if (!c->first_done) {
        c->first_done = true;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s.err, sizeof(s.err), fmt, ap);
        va_end(ap);
    }
}

/** 期望的端点（从冻结的注册需求推导，与实际描述符逐条对照） */
typedef struct {
    uint8_t addr;
    uint8_t attr;
    uint16_t mps;
    uint8_t interval;
    bool seen;
} exp_ep_t;

esp_err_t uc_selfcheck(uint32_t *checks_run, uint32_t *checks_failed)
{
    chk_t c = { 0, 0, false };

    if (!s.prepared) {
        esp_err_t err = uc_prepare();
        if (err != ESP_OK) {
            if (checks_run) { *checks_run = 0; }
            if (checks_failed) { *checks_failed = 1; }
            return err;
        }
    }
    s.err[0] = '\0';

    /* ---- device 描述符 ---- */
    chk(&c, s.dev[0] == LEN_DEVICE_DESC, "device: bLength=%u", s.dev[0]);
    chk(&c, s.dev[1] == DT_DEVICE, "device: bDescriptorType=%u", s.dev[1]);
    chk(&c, wu_rd16_local(s.dev + 2) >= 0x0210, "device: bcdUSB=%04X < 0x0210（主机不会取 BOS）",
        wu_rd16_local(s.dev + 2));
    chk(&c, s.dev[4] == 0xEF && s.dev[5] == 0x02 && s.dev[6] == 0x01,
        "device: bDeviceClass/Sub/Proto=%02X/%02X/%02X（复合设备必须是 EF/02/01）",
        s.dev[4], s.dev[5], s.dev[6]);
    chk(&c, s.dev[7] == EP0_MPS, "device: bMaxPacketSize0=%u", s.dev[7]);
    chk(&c, wu_rd16_local(s.dev + 8) == s.id.vid, "device: idVendor=%04X", wu_rd16_local(s.dev + 8));
    chk(&c, wu_rd16_local(s.dev + 10) == s.id.pid, "device: idProduct=%04X", wu_rd16_local(s.dev + 10));
    chk(&c, wu_rd16_local(s.dev + 10) != 0x4001 && wu_rd16_local(s.dev + 10) != 0x4002
             && wu_rd16_local(s.dev + 10) != 0x4003,
        "device: idProduct=%04X 落在 esp_tinyusb 自动 PID 空间（0x4000~0x407F）内，"
        "真机上可能撞到遗留设备节点的缓存驱动状态（见 frozen-allocation §3）",
        wu_rd16_local(s.dev + 10));
    chk(&c, s.dev[17] == 1, "device: bNumConfigurations=%u", s.dev[17]);

    /* ---- qualifier ---- */
    chk(&c, s.qual[0] == LEN_QUALIFIER_DESC && s.qual[1] == DT_DEVICE_QUALIFIER,
        "qualifier: 头字节 %02X %02X", s.qual[0], s.qual[1]);
    chk(&c, s.qual[4] == s.dev[4] && s.qual[5] == s.dev[5] && s.qual[6] == s.dev[6],
        "qualifier: 类字段与 device 不一致（USB 2.0 §9.6.2 要求逐字节相同）");

    /* ---- 每个速度的配置描述符 ---- */
    int itf_expected = 0;
    for (uint8_t i = 0; i < s.fn_count; i++) {
        itf_expected += s.alloc[i].itf_count;
    }

    for (int spd = 0; spd < 2; spd++) {
        const bool hs = (spd == 1);
        const uint8_t *cfg = s.cfg[spd];
        const uint32_t len = s.cfg_len[spd];
        const char *tag = hs ? "HS" : "FS";

        /* 期望端点表 */
        exp_ep_t exp[USB_COMPOSITE_MAX_FUNCTIONS * USB_COMPOSITE_MAX_EP_PER_DIR * 2];
        int n_exp = 0;
        for (uint8_t i = 0; i < s.fn_count; i++) {
            const usb_function_t *fn = s.fn[i];
            const usb_fn_alloc_t *a = &s.alloc[i];
            for (uint8_t k = 0; k < fn->ep_in_count; k++) {
                exp[n_exp].addr = a->ep_in[k];
                exp[n_exp].attr = (fn->ep_in_req[k].xfer == USB_EP_XFER_BULK)
                                  ? EP_XFER_BULK_ATTR : EP_XFER_INTERRUPT_ATTR;
                exp[n_exp].mps = hs ? fn->ep_in_req[k].mps_hs : fn->ep_in_req[k].mps_fs;
                exp[n_exp].interval = fn->ep_in_req[k].interval;
                exp[n_exp].seen = false;
                n_exp++;
            }
            for (uint8_t k = 0; k < fn->ep_out_count; k++) {
                exp[n_exp].addr = a->ep_out[k];
                exp[n_exp].attr = (fn->ep_out_req[k].xfer == USB_EP_XFER_BULK)
                                  ? EP_XFER_BULK_ATTR : EP_XFER_INTERRUPT_ATTR;
                exp[n_exp].mps = hs ? fn->ep_out_req[k].mps_hs : fn->ep_out_req[k].mps_fs;
                exp[n_exp].interval = fn->ep_out_req[k].interval;
                exp[n_exp].seen = false;
                n_exp++;
            }
        }

        chk(&c, cfg[0] == LEN_CONFIG_DESC && cfg[1] == DT_CONFIG, "%s cfg: 头字节 %02X %02X",
            tag, cfg[0], cfg[1]);
        chk(&c, wu_rd16_local(cfg + 2) == len, "%s cfg: wTotalLength=%u，实际 len=%u",
            tag, wu_rd16_local(cfg + 2), (unsigned)len);
        chk(&c, cfg[4] == (uint8_t)itf_expected, "%s cfg: bNumInterfaces=%u，期望 %d",
            tag, cfg[4], itf_expected);
        chk(&c, (cfg[7] & 0x80) != 0, "%s cfg: bmAttributes bit7 必须为 1", tag);
        chk(&c, cfg[8] == (uint8_t)((wc(&s.id) + 1) / 2), "%s cfg: bMaxPower=%u", tag, cfg[8]);

        /* 走一遍 */
        size_t off = LEN_CONFIG_DESC;
        int itf_seen = 0;
        int itf_alt_seen = 0;   /* 含备用设置在内的 Interface Descriptor 总数（诊断用） */
        int ep_seen = 0;
        bool iad_ncm_ok = false;
        bool ep_num_used_in[HS_EP_COUNT] = { false };
        bool ep_num_used_out[HS_EP_COUNT] = { false };

        while (off + 2 <= len) {
            uint8_t dlen = cfg[off];
            uint8_t dtype = cfg[off + 1];
            if (dlen < 2 || off + dlen > len) {
                chk(&c, false, "%s cfg: 偏移 %u 处 bLength=%u 越界", tag, (unsigned)off, dlen);
                break;
            }
            if (dtype == DT_INTERFACE) {
                chk(&c, dlen == LEN_ITF_DESC, "%s cfg: 接口描述符 bLength=%u", tag, dlen);
                /* ⚠️ 只有 bAlternateSetting == 0 才算"一个新接口"。
                 * NCM 数据接口必须有 alt=0/alt=1 两套（各 0 / 2 个端点），
                 * alt>0 复用同一个接口号，**不能**计入接口数（否则 itf_seen 会多算）。 */
                const uint8_t itf_num = cfg[off + 2];
                const uint8_t alt = cfg[off + 3];
                if (alt == 0) {
                    chk(&c, itf_num == (uint8_t)itf_seen,
                        "%s cfg: 接口号 %u 不连续（期望 %d）", tag, itf_num, itf_seen);
                    itf_seen++;
                } else {
                    chk(&c, itf_seen > 0 && itf_num == (uint8_t)(itf_seen - 1),
                        "%s cfg: 备用设置 alt=%u 的接口号 %u 不匹配上一个接口 %u",
                        tag, alt, itf_num, itf_seen > 0 ? (unsigned)(itf_seen - 1) : 0u);
                }
                itf_alt_seen++;
            } else if (dtype == DT_ENDPOINT) {
                chk(&c, dlen == LEN_EP_DESC, "%s cfg: 端点描述符 bLength=%u", tag, dlen);
                uint8_t addr = cfg[off + 2];
                uint8_t attr = cfg[off + 3] & 0x03;
                uint16_t mps = wu_rd16_local(cfg + off + 4) & 0x07FF;
                uint8_t interval = cfg[off + 6];
                int hit = -1;
                for (int e = 0; e < n_exp; e++) {
                    if (!exp[e].seen && exp[e].addr == addr) { hit = e; break; }
                }
                chk(&c, hit >= 0, "%s cfg: 端点 0x%02X 不在冻结分配表里（多出来的端点）", tag, addr);
                if (hit >= 0) {
                    exp[hit].seen = true;
                    chk(&c, attr == exp[hit].attr, "%s cfg: EP 0x%02X 类型 %u，期望 %u",
                        tag, addr, attr, exp[hit].attr);
                    chk(&c, mps == exp[hit].mps, "%s cfg: EP 0x%02X wMaxPacketSize=%u，期望 %u",
                        tag, addr, mps, exp[hit].mps);
                    if (exp[hit].attr == EP_XFER_INTERRUPT_ATTR) {
                        chk(&c, interval == exp[hit].interval, "%s cfg: EP 0x%02X interval=%u，期望 %u",
                            tag, addr, interval, exp[hit].interval);
                    }
                }
                uint8_t num = (uint8_t)(addr & 0x7F);
                if (addr & 0x80) {
                    chk(&c, !ep_num_used_in[num], "%s cfg: IN EP%u 重复", tag, num);
                    ep_num_used_in[num] = true;
                } else {
                    chk(&c, !ep_num_used_out[num], "%s cfg: OUT EP%u 重复", tag, num);
                    ep_num_used_out[num] = true;
                }
                ep_seen++;
            } else if (dtype == DT_IAD) {
                chk(&c, dlen == LEN_IAD_DESC, "%s cfg: IAD bLength=%u", tag, dlen);
                if (cfg[off + 2] == 3) {
                    chk(&c, cfg[off + 3] == 2 && cfg[off + 4] == 0x02 && cfg[off + 5] == 0x0D,
                        "%s cfg: NCM 的 IAD 应为 first=3 count=2 class=02/0D，实际 %u/%u/%02X/%02X",
                        tag, cfg[off + 2], cfg[off + 3], cfg[off + 4], cfg[off + 5]);
                    iad_ncm_ok = true;
                }
            }
            off += dlen;
        }

        chk(&c, off == len, "%s cfg: 解析到 %u 字节，声明 %u", tag, (unsigned)off, (unsigned)len);
        chk(&c, itf_seen == itf_expected, "%s cfg: 实际接口数 %d，期望 %d", tag, itf_seen, itf_expected);
        chk(&c, ep_seen == n_exp, "%s cfg: 实际端点数 %d，期望 %d", tag, ep_seen, n_exp);
        for (int e = 0; e < n_exp; e++) {
            chk(&c, exp[e].seen, "%s cfg: 冻结表里的端点 0x%02X 没有出现在描述符里", tag, exp[e].addr);
        }
        if (s.budget.itf_total == 5) {
            chk(&c, iad_ncm_ok, "%s cfg: 找不到 NCM 的 IAD（first=3 count=2）", tag);
        }
    }

    /* ---- 字符串 ---- */
    chk(&c, s.str_count <= USB_COMPOSITE_MAX_STR, "strings: %d 条超过上限 %d",
        s.str_count, USB_COMPOSITE_MAX_STR);
    chk(&c, s.str[0] == s_langid, "strings: 索引 0 必须是 LANGID 两字节");
    chk(&c, s.str[1] != NULL && s.str[2] != NULL && s.str[3] != NULL, "strings: 1/2/3 必须非空");
    for (int i = 4; i < s.str_count; i++) {
        chk(&c, s.str[i] != NULL, "strings: 索引 %d 为 NULL", i);
    }

    /* ---- BOS ---- */
    chk(&c, s.bos_len == LEN_BOS_TOTAL, "BOS: 总长 %u，期望 %u", s.bos_len, LEN_BOS_TOTAL);
    chk(&c, s.bos[1] == DT_BOS && s.bos[4] == 2, "BOS: 头 %02X caps=%u", s.bos[1], s.bos[4]);
    chk(&c, wu_rd16_local(s.bos + 2) == LEN_BOS_TOTAL, "BOS: wTotalLength=%u", wu_rd16_local(s.bos + 2));
    chk(&c, memcmp(s.bos + LEN_BOS + 4, k_webusb_uuid, 16) == 0, "BOS: WebUSB UUID 不对");
    chk(&c, s.bos[LEN_BOS + 22] == WEBUSB_VENDOR_CODE, "BOS: WebUSB bVendorCode=%u",
        s.bos[LEN_BOS + 22]);
    chk(&c, s.bos[LEN_BOS + 23] == 0, "BOS: WebUSB iLandingPage 必须为 0（否则 Chrome 会去取 GET_URL）");
    chk(&c, memcmp(s.bos + LEN_BOS + LEN_CAP_WEBUSB + 4, k_msos_uuid, 16) == 0,
        "BOS: MS OS 2.0 UUID 不对");
    chk(&c, wu_rd_local32(s.bos + LEN_BOS + LEN_CAP_WEBUSB + 20) == MSOS_WINDOWS_VERSION,
        "BOS: dwWindowsVersion=%08X", wu_rd_local32(s.bos + LEN_BOS + LEN_CAP_WEBUSB + 20));
    chk(&c, wu_rd16_local(s.bos + LEN_BOS + LEN_CAP_WEBUSB + 24) == s.msos_len,
        "BOS: wMSOSDescriptorSetTotalLength=%u，但描述符集实际 %u",
        wu_rd16_local(s.bos + LEN_BOS + LEN_CAP_WEBUSB + 24), s.msos_len);
    chk(&c, s.bos[LEN_BOS + 22] != s.bos[LEN_BOS + LEN_CAP_WEBUSB + 26],
        "BOS: WebUSB 与 MS OS 必须用不同的 bVendorCode（否则主机请求打架）");

    /* ---- MS OS 2.0 描述符集 ---- */
    {
        const uint8_t *m = s.msos;
        uint16_t set_len = wu_rd16_local(m + 8);
        chk(&c, wu_rd16_local(m + 0) == MSOS_SET_HEADER_LEN && wu_rd16_local(m + 2) == MSOS_SET_HEADER,
            "MSOS: Set Header 头字段不对");
        chk(&c, set_len == s.msos_len, "MSOS: Set Header wTotalLength=%u，实际 %u", set_len, s.msos_len);

        size_t o = MSOS_SET_HEADER_LEN;
#if CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET
        chk(&c, wu_rd16_local(m + o + 2) == MSOS_SUBSET_HEADER_CONFIG,
            "MSOS: 期望 Configuration Subset Header(0x0001)");
        o += MSOS_SUBSET_HEADER_LEN;
#endif
        /* 每个 wcid=true 的功能各占一个 Function Subset（T2 起 vendor 视频 IF1 也要 WinUSB） */
        int wcid_n = 0;
        for (uint8_t i = 0; i < s.fn_count; i++) {
            const usb_function_t *fn = s.fn[i];
            const usb_fn_alloc_t *a = &s.alloc[i];
            if (!fn->wcid) {
                continue;
            }
            wcid_n++;

            chk(&c, wu_rd16_local(m + o + 0) == MSOS_SUBSET_HEADER_LEN
                     && wu_rd16_local(m + o + 2) == MSOS_SUBSET_HEADER_FUNC,
                "MSOS: 期望 Function Subset Header(0x0002)，实际 type=%04X",
                wu_rd16_local(m + o + 2));
            chk(&c, m[o + 4] == a->itf[0],
                "MSOS: 功能 '%s' 的 Function Subset bFirstInterfaceNumber=%u，期望 %u",
                fn->name != NULL ? fn->name : "?", m[o + 4], a->itf[0]);
            chk(&c, wu_rd16_local(m + o + 6) == MSOS_SUBSET_LEN,
                "MSOS: 功能 '%s' 的 wSubsetLength=%u，期望 %u",
                fn->name != NULL ? fn->name : "?", wu_rd16_local(m + o + 6), MSOS_SUBSET_LEN);
            o += MSOS_SUBSET_HEADER_LEN;

            chk(&c, wu_rd16_local(m + o + 0) == MSOS_COMPAT_ID_LEN
                     && wu_rd16_local(m + o + 2) == MSOS_FEATURE_COMPAT_ID,
                "MSOS: 期望 CompatibleID(0x0003)");
            chk(&c, memcmp(m + o + 4, "WINUSB\0\0", 8) == 0, "MSOS: CompatibleID 不是 WINUSB");
            for (int k = 0; k < 8; k++) {
                chk(&c, m[o + 12 + k] == 0x00, "MSOS: SubCompatibleID 必须全 0");
            }
            o += MSOS_COMPAT_ID_LEN;

            chk(&c, wu_rd16_local(m + o + 0) == MSOS_REGPROP_LEN
                     && wu_rd16_local(m + o + 2) == MSOS_FEATURE_REG_PROP,
                "MSOS: 期望 Registry Property(0x0004)");
            chk(&c, wu_rd16_local(m + o + 4) == 0x0001,
                "MSOS: wPropertyDataType=%04X，**必须是 0x0001 (REG_SZ)**；"
                "0x0007(REG_MULTI_SZ) 在真机上会让 winusb.sys 起不来", wu_rd16_local(m + o + 4));
            chk(&c, wu_rd16_local(m + o + 6) == MSOS_REGPROP_NAME_LEN,
                "MSOS: wPropertyNameLength=%u，期望 %u（必须是单数 DeviceInterfaceGUID）",
                wu_rd16_local(m + o + 6), MSOS_REGPROP_NAME_LEN);
            chk(&c, memcmp(m + o + 8, "D\0e\0v\0i\0c\0e\0", 12) == 0, "MSOS: 属性名前缀不对");
            chk(&c, wu_rd16_local(m + o + 8 + MSOS_REGPROP_NAME_LEN) == MSOS_REGPROP_DATA_LEN,
                "MSOS: wPropertyDataLength=%u，期望 %u",
                wu_rd16_local(m + o + 8 + MSOS_REGPROP_NAME_LEN), MSOS_REGPROP_DATA_LEN);
            {
                static const char k_guid_le[] = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}";
                bool same = true;
                const char *g = k_guid_le;
                const uint8_t *p = m + o + 10 + MSOS_REGPROP_NAME_LEN;
                for (int k = 0; k < 38; k++) {
                    if (p[k * 2] != (uint8_t)g[k] || p[k * 2 + 1] != 0x00) { same = false; break; }
                }
                chk(&c, same, "MSOS: Registry Property 的值不是 {A5DCBF10-6530-11D2-901F-00C04FB951ED}"
                              "（Chrome/Edge 只认这一个 GUID）");
            }
            o += MSOS_REGPROP_LEN;
        }

        chk(&c, o == s.msos_len, "MSOS: 解析到 %u 字节，实际 %u", (unsigned)o, s.msos_len);
        {
            size_t expect = MSOS_SET_HEADER_LEN + (size_t)wcid_n * MSOS_SUBSET_LEN;
#if CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET
            expect += MSOS_SUBSET_HEADER_LEN;
#endif
            chk(&c, s.msos_len == expect,
                "MSOS: 总长 %u 与 %d 个 WCID 功能的预算 %u 不符",
                s.msos_len, wcid_n, (unsigned)expect);
        }
        chk(&c, wcid_n >= 1, "MSOS: 没有任何 WCID 功能（vendor 视频/控制接口都打不开）");
    }

    /* ---- 端点 / FIFO 预算 ---- */
    chk(&c, s.budget.ep_in_total <= HS_EP_IN_COUNT - 1,
        "预算: 用户 IN 端点 %u > %d", s.budget.ep_in_total, HS_EP_IN_COUNT - 1);
    chk(&c, s.budget.fifo_words_used <= HS_DFIFO_WORDS,
        "预算: DWC2 FIFO 需要 %u word > %u", s.budget.fifo_words_used, HS_DFIFO_WORDS);

    if (checks_run) { *checks_run = c.run; }
    if (checks_failed) { *checks_failed = c.fail; }
    if (c.fail != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s.err[0] = '\0';
    return ESP_OK;
}


const uint8_t *uc_dev_desc(uint32_t *len) { if (len) { *len = s.prepared ? LEN_DEVICE_DESC : 0; } return s.prepared ? s.dev : NULL; }
const uint8_t *uc_qual_desc(uint32_t *len) { if (len) { *len = s.prepared ? LEN_QUALIFIER_DESC : 0; } return s.prepared ? s.qual : NULL; }
const uint8_t *uc_cfg_desc(bool hs, uint32_t *len)
{
    int i = hs ? 1 : 0;
    if (len) { *len = s.prepared ? s.cfg_len[i] : 0; }
    return s.prepared ? s.cfg[i] : NULL;
}
const uint8_t *uc_bos_desc(uint32_t *len) { if (len) { *len = s.prepared ? s.bos_len : 0; } return s.prepared ? s.bos : NULL; }
const uint8_t *uc_msos_desc(uint32_t *len) { if (len) { *len = s.prepared ? s.msos_len : 0; } return s.prepared ? s.msos : NULL; }
const char *const *uc_str_desc(int *count) { if (count) { *count = s.prepared ? s.str_count : 0; } return s.prepared ? s.str : NULL; }
const usb_composite_budget_t *uc_budget(void) { return &s.budget; }
int uc_fn_count(void) { return (int)s.fn_count; }
const usb_function_t *uc_fn(int idx) { return (idx >= 0 && idx < (int)s.fn_count) ? s.fn[idx] : NULL; }
const usb_fn_alloc_t *uc_fn_alloc(int idx) { return (idx >= 0 && idx < (int)s.fn_count) ? &s.alloc[idx] : NULL; }

int uc_fn_by_vendor_idx(uint8_t vendor_idx)
{
    if (vendor_idx == USB_FN_NO_VENDOR_IDX) {
        return -1;
    }
    for (uint8_t i = 0; i < s.fn_count; i++) {
        if (s.alloc[i].vendor_idx == vendor_idx) {
            return (int)i;
        }
    }
    return -1;
}
