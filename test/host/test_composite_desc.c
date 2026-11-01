// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I1 复合层离线验证（verifier 独立编写，逐行核对冻结表）
//
// 被验证对象（**只读**）：components/usb_composite/usb_composite_desc.c
// 期望值来源          ：**docs/contracts/frozen-allocation.md 逐行手抄**（不向实现者索取期望值）
// 组装原料            ：本文件里的 4 个 mock `usb_function_t`（MTP / video / ctrl / NCM），
//                       它们只按冻结表申报接口/端点需求，**不硬编码任何号码**（号码只从
//                       `usb_fn_alloc_t` 取），以此检验装配器真的按注册意图分配。
//
// 断言面：
//   §1 接口号与类/子类/协议 5 行；§2 端点 9 行（FS 与 HS 分别校验）；IN 总数 ≤ 7
//   §2.2 DWC2 FIFO 预算 ≤ 1024 word（期望 852，由冻结表实算）
//   §3 身份（VID/PID/bcdUSB/bDeviceClass…/PID 必须在 esp_tinyusb 自动 PID 空间之外）
//   §4 字符串恰好 8 条、索引与内容与冻结表一致
//   §5 MS OS 2.0：单数 DeviceInterfaceGUID + REG_SZ(0x0001) + 指定 GUID、只给 IF2、
//      无 Configuration Subset（默认关，= 真机验证过的 166 B 形态）
//   §6 IAD：NCM bFirstInterface=3 / count=2 / 02-0D-00；单接口功能不得有 IAD
//   API 契约：prepare 幂等、prepare 后 register 必须 ESP_ERR_INVALID_STATE、selfcheck 通过
//
// 编译：gcc -std=c11 -Wall -Wextra -O1
//         -I components/usb_composite/include -I components/usb_composite -I test/host/shim
//         test/host/test_composite_desc.c components/usb_composite/usb_composite_desc.c -o <out>
// 运行：<out>                  （正常路径：期望全 PASS）
//       <out> --over-budget    （负例路径：超出 IN 预算/非法计数必须被拒绝）
//
// ⚠️ 边界：这是**离线**测试，只证明"装配器输出的字节与冻结表一致"。
//    **不能**证明真机枚举、FIFO 实际分配、Windows 驱动绑定成立。设备未连接。

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_composite.h"
#include "usb_composite_internal.h"

/* ---------------------------------------------------------------------------
 * 可选的集成模式（-DUSE_REAL_NCM=1）：把本文件的 mock NCM 换成
 * **WS-net 真实的** `usb_net_ncm_register()`（即真实 NCM 描述符片段）。
 *
 * 这样"真片段 × 真装配器/校验器"这一环也进入离线验证 —— **且本文件的所有期望值不变**，
 * 因此同一份期望同时约束 mock 与真片段（不给真片段开后门）。
 * 需要额外链接：components/usb_net/usb_net_ncm.c + test/host/real_ncm_bridge.c（提供 g_usbnet）
 *             + components/usb_net/test/host/stubs（esp_err/esp_log/esp_netif/sdkconfig 替身）。
 * ------------------------------------------------------------------------- */
#ifdef USE_REAL_NCM
extern esp_err_t usb_net_ncm_register(void);
#endif

/* =====================================================================
 * 断言
 * ===================================================================== */

static int g_pass = 0, g_fail = 0;
static const char *g_ctx = "";

static void ctx_set(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_ctx = buf;
}

/** 断言；返回值 = cond，便于写 "if (!ck(...)) return;" 的短路 */
static int ck(int cond, const char *fmt, ...)
{
    if (cond) {
        g_pass++;
        return 1;
    }
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    printf("  [FAIL] %s : ", g_ctx);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    return 0;
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  [info] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* =====================================================================
 * 描述符常量（只抄 USB 规范/MS OS 2.0 的公开值，不抄实现）
 * ===================================================================== */

#define DT_DEVICE    0x01
#define DT_CONFIG    0x02
#define DT_STRING    0x03
#define DT_INTERFACE 0x04
#define DT_ENDPOINT  0x05
#define DT_IAD       0x0B
#define DT_BOS       0x0F
#define DT_DEV_CAP   0x10

#define XFER_BULK 0x02
#define XFER_INT  0x03

#define USBCFG_ITF_CLASS_MTP      0x06
#define USBCFG_ITF_SUBCLASS_MTP   0x01
#define USBCFG_ITF_PROTO_MTP      0x01
#define USBCFG_ITF_CLASS_VENDOR   0xFF
#define USBCFG_ITF_CLASS_NCM_CTRL 0x02
#define USBCFG_ITF_SUB_NCM_CTRL   0x0D
#define USBCFG_ITF_CLASS_NCM_DATA 0x0A
/** 冻结 §1（2026-10 更正）：IF4 = `0A/00/01`（CDC-NCM 1.0 Table 4.3 = Network Transfer Block） */
#define CDC_DATA_PROTO_NTB 0x01

/* 16 字节平台能力 UUID（MS 公开常量；与批 0 归档 ELF dump 逐字节一致） */
static const uint8_t UUID_WEBUSB[16] = {
    0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
    0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65
};
static const uint8_t UUID_MSOS20[16] = {
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F
};

/* frozen-allocation §5：GUID 值（单数 REG_SZ） */
static const char GUID_USB_DEVICE[] = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}";

/* frozen-allocation §2.2 的 FIFO 预算 —— **verifier 独立复算**（不照抄该节的数字）：
 *
 *   dwc2_esp32.h:80     P4 HS 口: ep_count=16, ep_in_count=8, otg_dfifo_depth=1024
 *   esp_tinyusb/tusb_config.h:110   CFG_TUD_DWC2_DMA_ENABLE = 1
 *   tinyusb/tusb_option.h:310       CFG_TUD_DWC2_SLAVE_ENABLE_DEFAULT = !DMA = 0 → 只走 DMA
 *   dcd_dwc2.c:34-35                DMA/SLAVE 两者都关时 #error → 设备栈**必须**走 DMA
 *   dcd_dwc2.c:139                  dma_device_enabled() = DMA && ghwcfg2.arch == INTERNAL_DMA
 *   → 批 0 真机枚举成功（probe-report §2.1/§2.2）⇒ arch 必为 INTERNAL_DMA ⇒ is_dma = true
 *   dcd_dwc2.c:291-294              is_dma ⇒ dfifo_top = 1024 - 2*ep_count = 1024 - 32 = 992
 *                                   （**EPInfo / DMA 元数据 32 word**）
 *   dcd_dwc2.c:232                  GRXFSIZ = 13+1+2*(512/4+1)+2*16 = 304
 *   dcd_dwc2.c:244                  TX FIFO = ceil(mps/4)：EP0 16 + EP1 16 + EP2/3/4/6 各 128 + EP5 4 = 548
 *   合计 = 32 + 304 + 548 = 884 word ≤ 1024；余 = (1024-32-548) - 304 = 444 - 304 = 140 word
 *
 *   ⚠️ frozen-allocation §2.2 写的是 852 / 余 172 —— 它**漏了 EPInfo 的 32 word**。
 *      该缺陷已作为对抗性发现报 Lead（见 docs/verification）；本测试断言**独立复算值 884**。
 *      结论不变（884 ≤ 1024，方案可行；开 double buffering 后 884+4×128=1396 > 1024，仍不可开）。
 */
#define EXPECT_FIFO_WORDS_USED 884
#define EXPECT_FIFO_WORDS_FREE 140
#define EXPECT_FIFO_WORDS_TOTAL 1024
#define EXPECT_MSOS_LEN 322               /* 0x0A + 2 × (0x08 + 0x14 + 0x80) */
#define EXPECT_MSOS_SUBSET_LEN 156        /* 0x08 + 0x14 + 0x80 */
#define EXPECT_MSOS_REGPROP_LEN 128       /* 0x80 */
#define EXPECT_MSOS_WCID_N 2              /* IF1 视频 + IF2 控制（2026-10-22 T2 裁决） */
#define EXPECT_BOS_LEN 57

/* =====================================================================
 * 描述符片段小工具（mock 功能用它产出自己的 Interface/Endpoint 描述符）
 * ===================================================================== */

static size_t emit_iface(uint8_t *b, uint8_t num, uint8_t alt, uint8_t n_ep,
                         uint8_t cls, uint8_t sub, uint8_t proto, uint8_t str)
{
    b[0] = 9; b[1] = DT_INTERFACE; b[2] = num; b[3] = alt; b[4] = n_ep;
    b[5] = cls; b[6] = sub; b[7] = proto; b[8] = str;
    return 9;
}

static size_t emit_ep(uint8_t *b, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval)
{
    b[0] = 7; b[1] = DT_ENDPOINT; b[2] = addr; b[3] = attr;
    b[4] = (uint8_t)(mps & 0xFF); b[5] = (uint8_t)(mps >> 8); b[6] = interval;
    return 7;
}

/* 小端读工具 */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** ASCII -> UTF-16LE（含结尾 NUL） */
static size_t utf16le(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s) + 1;
    if (n * 2 > cap) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = (uint8_t)s[i];
        out[2 * i + 1] = 0;
    }
    return n * 2;
}

/* =====================================================================
 * 四个 mock 功能（接口/端点需求逐条抄 frozen-allocation §1/§2）
 * ===================================================================== */

/* §1/§2 MTP：IF0 06/01/01；EP1 IN int 64/64 poll 1；EP2 IN bulk 512/64；EP2 OUT bulk 512/64 */
static esp_err_t mtp_build(bool hs, const usb_fn_alloc_t *a, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t o = 0;
    o += emit_iface(buf + o, a->itf[0], 0, 3, USBCFG_ITF_CLASS_MTP, USBCFG_ITF_SUBCLASS_MTP,
                    USBCFG_ITF_PROTO_MTP, a->itf_str_idx);
    o += emit_ep(buf + o, a->ep_in[0], XFER_INT, 64, 1);
    o += emit_ep(buf + o, a->ep_in[1], XFER_BULK, hs ? 512 : 64, 0);
    o += emit_ep(buf + o, a->ep_out[0], XFER_BULK, hs ? 512 : 64, 0);
    (void)cap;
    *out_len = o;
    return ESP_OK;
}

/* §1/§2 Vendor 视频：IF1 FF/00/00；只有 EP3 IN bulk 512/64 */
static esp_err_t video_build(bool hs, const usb_fn_alloc_t *a, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t o = 0;
    o += emit_iface(buf + o, a->itf[0], 0, 1, USBCFG_ITF_CLASS_VENDOR, 0x00, 0x00, a->itf_str_idx);
    o += emit_ep(buf + o, a->ep_in[0], XFER_BULK, hs ? 512 : 64, 0);
    (void)cap;
    *out_len = o;
    return ESP_OK;
}

/* §1/§2 Vendor 控制：IF2 FF/00/00；EP4 IN bulk 512/64 + EP4 OUT bulk 512/64 */
static esp_err_t ctrl_build(bool hs, const usb_fn_alloc_t *a, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t o = 0;
    o += emit_iface(buf + o, a->itf[0], 0, 2, USBCFG_ITF_CLASS_VENDOR, 0x00, 0x00, a->itf_str_idx);
    o += emit_ep(buf + o, a->ep_in[0], XFER_BULK, hs ? 512 : 64, 0);
    o += emit_ep(buf + o, a->ep_out[0], XFER_BULK, hs ? 512 : 64, 0);
    (void)cap;
    *out_len = o;
    return ESP_OK;
}

/* §1/§2/§6 NCM：IF3 02/0D/00（控制，1 EP）+ IF4 0A/00/00（数据，alt0/alt1）；
 * EP5 IN int 16/16 poll 8（通知）；EP6 IN/OUT bulk 512/64（数据）；IAD 由复合层插入 */
#ifndef USE_REAL_NCM
/* 集成模式下改用 ws-net 的真实片段，本 mock 不参与编译 */
static esp_err_t ncm_build(bool hs, const usb_fn_alloc_t *a, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t o = 0;
    o += emit_iface(buf + o, a->itf[0], 0, 1, USBCFG_ITF_CLASS_NCM_CTRL, USBCFG_ITF_SUB_NCM_CTRL,
                    0x00, a->itf_str_idx);
    /* CDC Header / Union / Ethernet / NCM functional */
    {
        uint8_t *b = buf + o;
        b[0] = 5; b[1] = 0x24; b[2] = 0x00; b[3] = 0x10; b[4] = 0x01;  o += 5;
        b = buf + o;
        b[0] = 5; b[1] = 0x24; b[2] = 0x06; b[3] = a->itf[0]; b[4] = a->itf[1]; o += 5;
        b = buf + o;
        b[0] = 13; b[1] = 0x24; b[2] = 0x0F; b[3] = a->extra_str_idx;
        b[4] = 0; b[5] = 0; b[6] = 0; b[7] = 0; b[8] = 0; b[9] = 0; b[10] = 0; b[11] = 0; b[12] = 0;
        o += 13;
        b = buf + o;
        b[0] = 6; b[1] = 0x24; b[2] = 0x1A; b[3] = 0x00; b[4] = 0x01; b[5] = 0x21; o += 6;
    }
    o += emit_ep(buf + o, a->ep_in[0], XFER_INT, 16, 8);          /* EP5 通知 */
    /* 数据接口：**两套 alternate setting**（冻结 §1.1：NCM 数据接口必须两套，
     * alt0 = 0 个端点（默认、不占带宽），alt1 = 2 个 bulk 端点；两者**复用同一接口号**，
     * 且 alt != 0 **不计入** bNumInterfaces）。这正是标准 `TUD_CDC_NCM_DESCRIPTOR` 的形态。 */
    o += emit_iface(buf + o, a->itf[1], 0, 0, USBCFG_ITF_CLASS_NCM_DATA, 0x00, CDC_DATA_PROTO_NTB, 0x00);
    o += emit_iface(buf + o, a->itf[1], 1, 2, USBCFG_ITF_CLASS_NCM_DATA, 0x00, CDC_DATA_PROTO_NTB, 0x00);
    o += emit_ep(buf + o, a->ep_in[1], XFER_BULK, hs ? 512 : 64, 0);   /* EP6 IN */
    o += emit_ep(buf + o, a->ep_out[0], XFER_BULK, hs ? 512 : 64, 0);  /* EP6 OUT */
    (void)cap;
    *out_len = o;
    return ESP_OK;
}
#endif /* !USE_REAL_NCM */

/* =====================================================================
 * 注册表（注册顺序 MTP -> video -> ctrl -> NCM，见 Lead 给的注册形态表）
 * ===================================================================== */

static const usb_function_t FN_MTP = {
    .name = "mtp",
    .itf_count = 1,
    .itf_string = "MeidaP4 SD Card",      /* §4 idx 5 */
    .extra_string = NULL,
    .use_iad = false,
    .ep_in_req = { { USB_EP_XFER_INTERRUPT, 64, 64, 1 }, { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_in_count = 2,
    .ep_out_req = { { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_out_count = 1,
    .ep_out_same_num_as_in = { 1 },       /* -> 用第 1 个 IN（EP2）的号码 = EP2 OUT */
    .is_vendor = false,
    .build_desc = mtp_build,
    .wcid = false,
};

static const usb_function_t FN_VIDEO = {
    .name = "video",
    .itf_count = 1,
    .itf_string = "MeidaP4 WebUSB",       /* §4 idx 4（与 ctrl 共用） */
    .extra_string = NULL,
    .use_iad = false,
    .ep_in_req = { { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_in_count = 1,
    .ep_out_req = { { USB_EP_XFER_BULK, 512, 64, 0 } },   /* 冻结表：视频只有 IN -> count 0 */
    .ep_out_count = 0,
    .ep_out_same_num_as_in = { USB_EP_NO_PAIR },
    .is_vendor = true,
    .build_desc = video_build,
    /* §5（2026-10-22 T2）：视频接口也发 WCID —— Windows 只给 subset 里的接口装 winusb.sys，
       否则 MI_01 无驱动、Chrome/主机 WinUSB 打不开 EP3（真机实测确认）。 */
    .wcid = true,
    .wcid_compatible_id = "WINUSB",
    .wcid_device_interface_guid = GUID_USB_DEVICE,
};

static const usb_function_t FN_CTRL = {
    .name = "ctrl",
    .itf_count = 1,
    .itf_string = "MeidaP4 WebUSB",       /* §4 idx 4（共用） */
    .extra_string = NULL,
    .use_iad = false,
    .ep_in_req = { { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_in_count = 1,
    .ep_out_req = { { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_out_count = 1,
    .ep_out_same_num_as_in = { 0 },       /* -> EP4 OUT */
    .is_vendor = true,
    .build_desc = ctrl_build,
    .wcid = true,                          /* §5：T2 起 IF1/IF2 都带 WCID */
    .wcid_compatible_id = "WINUSB",
    .wcid_device_interface_guid = GUID_USB_DEVICE,
};

#ifndef USE_REAL_NCM
static const usb_function_t FN_NCM = {
    .name = "ncm",
    .itf_count = 2,
    .itf_string = "MeidaP4 Network",      /* §4 idx 6 */
    .extra_string = "AABBCCDDEEFF",       /* §4 idx 7（NCM MAC 串） */
    .use_iad = true,                       /* §6 */
    .ep_in_req = { { USB_EP_XFER_INTERRUPT, 16, 16, 8 }, { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_in_count = 2,
    .ep_out_req = { { USB_EP_XFER_BULK, 512, 64, 0 } },
    .ep_out_count = 1,
    .ep_out_same_num_as_in = { 1 },       /* -> 用第 1 个 IN（EP6）的号码 = EP6 OUT */
    .is_vendor = false,
    .build_desc = ncm_build,
    .wcid = false,
};
#endif /* !USE_REAL_NCM */

/* =====================================================================
 * 离线替身：`usb_composite_identity_default()`
 *
 * 真实定义在 `components/usb_composite/usb_composite.c`（IDF 侧，无法原生编译）。
 * 这里按 **frozen-allocation §3 逐条** 写替身，让 desc.c 的"未显式设 identity 时用默认值"
 * 这条路径可离线执行。
 *
 * ⚠️ 边界：替身只能证明"desc.c 用了被给定的 identity"；"真实默认 identity == 冻结值"
 *    这一条由 `usb_composite.c:76-95` 的源码核对覆盖（见 docs/verification 报告），
 *    或在真机上核实。替身值出错不会让断言更宽松（期望值仍来自冻结表）。
 * ===================================================================== */
void usb_composite_identity_default(usb_composite_identity_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->vid = 0x303A;
    out->pid = 0x4103;                          /* §3：刻意在 0x4000~0x407F 之外 */
    out->bcd_usb = 0x0210;                      /* §3：>= 0x0210 */
    out->bcd_device = 0x0110;                   /* §3：起始值 + T2 递增（IF1 WCID） */
    out->b_device_class = 0xEF;
    out->b_device_subclass = 0x02;
    out->b_device_protocol = 0x01;
    out->self_powered = false;
    out->vbus_monitor_io = -1;
    out->max_power_ma = 500;
}

/* =====================================================================
 * 描述符解析器（raw bytes，不信任何导出的结构体）
 * ===================================================================== */

#define MAX_ITF 16
#define MAX_EP 32

typedef struct { uint8_t num, alt, n_ep, cls, sub, proto, str; } itf_t;
typedef struct { uint8_t addr, attr, interval; uint16_t mps; int itf; uint8_t alt; } ep_t;
typedef struct { uint8_t first, count, cls, sub, proto, len; } iad_t;

typedef struct {
    uint16_t wtotal;
    uint8_t num_itf, cfg_val, attr, power, str;
    int itf_n, ep_n, iad_n;
    itf_t itf[MAX_ITF];
    ep_t ep[MAX_EP];
    iad_t iad[4];
} cfg_t;

static int parse_cfg(const uint8_t *d, size_t len, cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    if (len < 9 || d[0] != 9 || d[1] != DT_CONFIG) {
        return -1;
    }
    c->wtotal = rd16(d + 2);
    c->num_itf = d[4];
    c->cfg_val = d[5];
    c->str = d[6];
    c->attr = d[7];
    c->power = d[8];
    if (c->wtotal != len) {
        return -2;
    }
    size_t o = 9;
    int cur = -1;
    while (o + 2 <= len) {
        uint8_t bl = d[o], bt = d[o + 1];
        if (bl < 2 || o + bl > len) {
            return -3;
        }
        if (bt == DT_INTERFACE) {
            if (c->itf_n >= MAX_ITF) {
                return -4;
            }
            cur = c->itf_n++;
            c->itf[cur].num = d[o + 2];
            c->itf[cur].alt = d[o + 3];
            c->itf[cur].n_ep = d[o + 4];
            c->itf[cur].cls = d[o + 5];
            c->itf[cur].sub = d[o + 6];
            c->itf[cur].proto = d[o + 7];
            c->itf[cur].str = d[o + 8];
        } else if (bt == DT_ENDPOINT) {
            if (c->ep_n >= MAX_EP || cur < 0) {
                return -5;
            }
            ep_t *e = &c->ep[c->ep_n++];
            e->addr = d[o + 2];
            e->attr = d[o + 3];
            e->mps = rd16(d + o + 4);
            e->interval = d[o + 6];
            e->itf = cur;
            e->alt = c->itf[cur].alt;
        } else if (bt == DT_IAD) {
            if (c->iad_n >= 4) {
                return -6;
            }
            iad_t *a = &c->iad[c->iad_n++];
            a->len = bl;
            a->first = d[o + 2];
            a->count = d[o + 3];
            a->cls = d[o + 4];
            a->sub = d[o + 5];
            a->proto = d[o + 6];
        }
        o += bl;
    }
    return 0;
}

static int itf_index(const cfg_t *c, uint8_t num, uint8_t alt)
{
    for (int i = 0; i < c->itf_n; i++) {
        if (c->itf[i].num == num && c->itf[i].alt == alt) {
            return i;
        }
    }
    return -1;
}

static int ep_count_addr(const cfg_t *c, uint8_t addr)
{
    int n = 0;
    for (int i = 0; i < c->ep_n; i++) {
        if (c->ep[i].addr == addr) {
            n++;
        }
    }
    return n;
}

static const ep_t *ep_find(const cfg_t *c, uint8_t addr)
{
    for (int i = 0; i < c->ep_n; i++) {
        if (c->ep[i].addr == addr) {
            return &c->ep[i];
        }
    }
    return NULL;
}

/** 断言行 [iface num] 上存在 addr 端点，且方向/类型/wMaxPacketSize/轮询与冻结表一致 */
static void check_ep(const cfg_t *c, const char *speed, uint8_t itf_num, uint8_t addr,
                     uint8_t attr, uint16_t mps, uint8_t interval)
{
    int n = ep_count_addr(c, addr);
    if (!ck(n == 1, "%s 端点 0x%02X 出现 %d 次（期望 1 次）", speed, addr, n)) {
        return;
    }
    const ep_t *e = ep_find(c, addr);
    ck(c->itf[e->itf].num == itf_num, "%s 端点 0x%02X 挂在 IF%u 上（期望 IF%u）", speed, addr,
       c->itf[e->itf].num, itf_num);
    ck((e->attr & 0x03) == attr, "%s 端点 0x%02X 类型=0x%02X（期望 0x%02X：%s）", speed, addr,
       e->attr, attr, attr == XFER_BULK ? "bulk" : "interrupt");
    ck(e->mps == mps, "%s 端点 0x%02X wMaxPacketSize=%u（期望 %u）", speed, addr, e->mps, mps);
    if (attr == XFER_INT) {
        ck(e->interval == interval, "%s 端点 0x%02X bInterval=%u（期望 %u）", speed, addr,
           e->interval, interval);
    } else {
        ck(e->interval == 0, "%s 端点 0x%02X 是 bulk，bInterval 应为 0（实测 %u）", speed, addr,
           e->interval);
    }
}

static void check_iface(const cfg_t *c, const char *speed, uint8_t num, uint8_t alt, uint8_t n_ep,
                        uint8_t cls, uint8_t sub, uint8_t proto, int str_check, uint8_t str_exp)
{
    int i = itf_index(c, num, alt);
    if (!ck(i >= 0, "%s 缺少接口 IF%u alt%u", speed, num, alt)) {
        return;
    }
    ck(c->itf[i].n_ep == n_ep, "%s IF%u alt%u bNumEndpoints=%u（期望 %u）", speed, num, alt,
       c->itf[i].n_ep, n_ep);
    ck(c->itf[i].cls == cls && c->itf[i].sub == sub && c->itf[i].proto == proto,
       "%s IF%u alt%u 类/子类/协议=%02X/%02X/%02X（期望 %02X/%02X/%02X）", speed, num, alt,
       c->itf[i].cls, c->itf[i].sub, c->itf[i].proto, cls, sub, proto);
    if (str_check) {
        ck(c->itf[i].str == str_exp, "%s IF%u alt%u iInterface=%u（期望 %u）", speed, num, alt,
           c->itf[i].str, str_exp);
    }
}

/* =====================================================================
 * 正常路径
 * ===================================================================== */

static void check_config(const char *speed, bool hs)
{
    ctx_set("§1/§2 配置描述符（%s）", speed);
    uint32_t len = 0;
    const uint8_t *d = uc_cfg_desc(hs, &len);
    if (!ck(d != NULL && len > 9, "%s 配置描述符为空", speed)) {
        return;
    }
    cfg_t c;
    int rc = parse_cfg(d, len, &c);
    ck(rc == 0, "%s 配置描述符解析失败（rc=%d）", speed, rc);
    if (rc != 0) {
        return;
    }
    info("%s 配置描述符 %u 字节：IF 段=%d 端点=%d IAD=%d", speed, len, c.itf_n, c.ep_n, c.iad_n);

    /* --- 配置头 --- */
    ck(c.cfg_val == 1, "%s bConfigurationValue=%u（期望 1）", speed, c.cfg_val);
    ck(c.num_itf == 5, "%s bNumInterfaces=%u（冻结 §1：5 个接口）", speed, c.num_itf);
    ck((c.attr & 0x80) != 0, "%s bmAttributes=0x%02X 必须置 bit7（保留位=1）", speed, c.attr);
    ck(c.power <= 250, "%s bMaxPower=%u（= %u mA，必须 <= 500 mA）", speed, c.power,
       c.power * 2);

    /* --- §1 五行接口（字符串索引按冻结 §4 2026-10 更正：4=MTP / 5=WebUSB 共享 / 6=NCM） --- */
    check_iface(&c, speed, 0, 0, 3, 0x06, 0x01, 0x01, 1, 4);   /* IF0 MTP，str=4 */
    check_iface(&c, speed, 1, 0, 1, 0xFF, 0x00, 0x00, 1, 5);   /* IF1 视频，str=5 */
    check_iface(&c, speed, 2, 0, 2, 0xFF, 0x00, 0x00, 1, 5);   /* IF2 控制，str=5（共用） */
    check_iface(&c, speed, 3, 0, 1, 0x02, 0x0D, 0x00, 1, 6);   /* IF3 NCM 控制，str=6 */
    /* §1.1：NCM 数据接口两套 alt（alt0 无端点、alt1 两个端点；复用同一接口号、不计入 bNumInterfaces） */
    check_iface(&c, speed, 4, 0, 0, 0x0A, 0x00, CDC_DATA_PROTO_NTB, 0, 0);
    check_iface(&c, speed, 4, 1, 2, 0x0A, 0x00, CDC_DATA_PROTO_NTB, 0, 0);

    /* --- §1.1：alt != 0 必须复用前一个接口号（不许出现第 6 个接口号） --- */
    {
        for (int i = 1; i < c.itf_n; i++) {
            if (c.itf[i].alt != 0) {
                ck(c.itf[i].num == c.itf[i - 1].num,
                   "%s alt=%u 的接口号 %u != 前一个接口号 %u（§1.1 要求复用）", speed,
                   c.itf[i].alt, c.itf[i].num, c.itf[i - 1].num);
            }
        }
        int max_num = -1;
        for (int i = 0; i < c.itf_n; i++) {
            if (c.itf[i].num > max_num) max_num = c.itf[i].num;
        }
        ck(max_num == 4, "%s 最大接口号=%d（冻结 §1：4 ⇒ 共 5 个接口；alt 不得新增接口号）", speed, max_num);
    }

    /* --- §2 九行端点（FS/HS 分别校验） --- */
    const uint16_t bulk = hs ? 512 : 64;
    check_ep(&c, speed, 0, 0x80 | 1, XFER_INT, 64, 1);        /* EP1 IN int 64/64 poll 1 */
    check_ep(&c, speed, 0, 0x80 | 2, XFER_BULK, bulk, 0);     /* EP2 IN bulk */
    check_ep(&c, speed, 0, 0x02,     XFER_BULK, bulk, 0);     /* EP2 OUT bulk */
    check_ep(&c, speed, 1, 0x80 | 3, XFER_BULK, bulk, 0);     /* EP3 IN bulk（视频独占） */
    check_ep(&c, speed, 2, 0x80 | 4, XFER_BULK, bulk, 0);     /* EP4 IN bulk */
    check_ep(&c, speed, 2, 0x04,     XFER_BULK, bulk, 0);     /* EP4 OUT bulk */
    check_ep(&c, speed, 3, 0x80 | 5, XFER_INT, 16, 8);        /* EP5 IN int 16/16 poll 8 */
    check_ep(&c, speed, 4, 0x80 | 6, XFER_BULK, bulk, 0);     /* EP6 IN bulk */
    check_ep(&c, speed, 4, 0x06,     XFER_BULK, bulk, 0);     /* EP6 OUT bulk */

    /* --- 端点号唯一性 / IN ≤ 7 --- */
    {
        int in_n = 0, out_n = 0;
        for (int i = 0; i < c.ep_n; i++) {
            if (c.ep[i].addr & 0x80) {
                in_n++;
            } else {
                out_n++;
            }
            for (int j = i + 1; j < c.ep_n; j++) {
                ck(c.ep[i].addr != c.ep[j].addr, "%s 端点地址 0x%02X 重复", speed, c.ep[i].addr);
            }
        }
        ck(in_n == 6, "%s IN 端点数=%d（冻结 §2：EP1~EP6 = 6）", speed, in_n);
        ck(in_n <= 7, "%s IN 端点数=%d 超过 7（HS 口用户 IN 上限）", speed, in_n);
        ck(out_n == 3, "%s OUT 端点数=%d（冻结 §2：EP2/EP4/EP6 = 3）", speed, out_n);
    }

    /* --- §6 IAD --- */
    ck(c.iad_n == 1, "%s IAD 数量=%d（只有 NCM 需要 1 个）", speed, c.iad_n);
    if (c.iad_n == 1) {
        ck(c.iad[0].len == 8, "%s IAD bLength=%u（期望 8）", speed, c.iad[0].len);
        ck(c.iad[0].first == 3, "%s IAD bFirstInterfaceNumber=%u（冻结 §6：3）", speed, c.iad[0].first);
        ck(c.iad[0].count == 2, "%s IAD bInterfaceCount=%u（冻结 §6：2）", speed, c.iad[0].count);
        ck(c.iad[0].cls == 0x02 && c.iad[0].sub == 0x0D && c.iad[0].proto == 0x00,
           "%s IAD 类/子类/协议=%02X/%02X/%02X（期望 02/0D/00）", speed, c.iad[0].cls,
           c.iad[0].sub, c.iad[0].proto);
    }

    /* --- 顺序：IAD 必须紧贴 IF3 之前（usbccgp 靠它分发） --- */
    {
        size_t o = 9;
        int iad_off = -1, if3_off = -1;
        while (o + 2 <= len) {
            if (d[o + 1] == DT_IAD && iad_off < 0) {
                iad_off = (int)o;
            }
            if (d[o + 1] == DT_INTERFACE && d[o + 2] == 3 && if3_off < 0) {
                if3_off = (int)o;
            }
            o += d[o];
        }
        ck(iad_off >= 0 && if3_off == iad_off + 8, "%s IAD 未紧贴 IF3 之前（iad@%d if3@%d）", speed,
           iad_off, if3_off);
    }
}

static void check_alloc_table(void)
{
    ctx_set("§2.1 端点号分配（按注册意图）");
    ck(uc_fn_count() == 4, "已注册功能数=%d（期望 4）", uc_fn_count());

    const usb_fn_alloc_t *a0 = uc_fn_alloc(0);   /* MTP */
    const usb_fn_alloc_t *a1 = uc_fn_alloc(1);   /* video */
    const usb_fn_alloc_t *a2 = uc_fn_alloc(2);   /* ctrl */
    const usb_fn_alloc_t *a3 = uc_fn_alloc(3);   /* NCM */
    if (!ck(a0 && a1 && a2 && a3, "uc_fn_alloc() 返回空")) {
        return;
    }

    /* 接口号 */
    ck(a0->itf[0] == 0 && a0->itf_count == 1, "MTP 接口分配 IF%u（期望 IF0）", a0->itf[0]);
    ck(a1->itf[0] == 1 && a1->itf_count == 1, "video 接口分配 IF%u（期望 IF1）", a1->itf[0]);
    ck(a2->itf[0] == 2 && a2->itf_count == 1, "ctrl 接口分配 IF%u（期望 IF2）", a2->itf[0]);
    ck(a3->itf[0] == 3 && a3->itf[1] == 4 && a3->itf_count == 2,
       "NCM 接口分配 IF%u/IF%u（期望 IF3/IF4）", a3->itf[0], a3->itf[1]);

    /* IN 号：注册顺序递增 EP1..EP6 */
    ck(a0->ep_in_count == 2 && a0->ep_in[0] == (0x80 | 1) && a0->ep_in[1] == (0x80 | 2),
       "MTP IN = 0x%02X/0x%02X（期望 0x81/0x82）", a0->ep_in[0], a0->ep_in[1]);
    ck(a1->ep_in_count == 1 && a1->ep_in[0] == (0x80 | 3), "video IN = 0x%02X（期望 0x83）",
       a1->ep_in[0]);
    ck(a2->ep_in_count == 1 && a2->ep_in[0] == (0x80 | 4), "ctrl IN = 0x%02X（期望 0x84）",
       a2->ep_in[0]);
    ck(a3->ep_in_count == 2 && a3->ep_in[0] == (0x80 | 5) && a3->ep_in[1] == (0x80 | 6),
       "NCM IN = 0x%02X/0x%02X（期望 0x85/0x86）", a3->ep_in[0], a3->ep_in[1]);

    /* OUT 号：由显式配对表指向对应 IN 的号码 */
    ck(a0->ep_out_count == 1 && a0->ep_out[0] == 0x02, "MTP OUT = EP%u（期望 EP2，配对第 1 个 IN）",
       a0->ep_out[0]);
    ck(a1->ep_out_count == 0, "video OUT 数=%u（冻结表：视频只有 IN）", a1->ep_out_count);
    ck(a2->ep_out_count == 1 && a2->ep_out[0] == 0x04, "ctrl OUT = EP%u（期望 EP4）", a2->ep_out[0]);
    ck(a3->ep_out_count == 1 && a3->ep_out[0] == 0x06, "NCM OUT = EP%u（期望 EP6）", a3->ep_out[0]);

    /* 字符串索引（§4，2026-10 更正后：按注册顺序 MTP→video→ctrl→NCM） */
    ck(a0->itf_str_idx == 4, "MTP itf_str_idx=%u（§4：4 = MeidaP4 SD Card）", a0->itf_str_idx);
    ck(a1->itf_str_idx == 5, "video itf_str_idx=%u（§4：5 = MeidaP4 WebUSB）", a1->itf_str_idx);
    ck(a2->itf_str_idx == 5, "ctrl itf_str_idx=%u（§4：5，与 IF1 内容相同 → 共享）", a2->itf_str_idx);
    ck(a3->itf_str_idx == 6, "NCM itf_str_idx=%u（§4：6）", a3->itf_str_idx);
    ck(a3->extra_str_idx == 7, "NCM extra_str_idx=%u（§4：7 = MAC 串）", a3->extra_str_idx);
    ck(a0->extra_str_idx == 0 && a1->extra_str_idx == 0 && a2->extra_str_idx == 0,
       "非 NCM 功能的 extra_str_idx 应为 0（实测 %u/%u/%u）", a0->extra_str_idx, a1->extra_str_idx,
       a2->extra_str_idx);

    /* vendor_idx（§1：只有 IF1/IF2 是 vendor 功能，序号按注册顺序 0/1） */
    ck(a0->vendor_idx == USB_FN_NO_VENDOR_IDX, "MTP vendor_idx=%u（期望 0xFF）", a0->vendor_idx);
    ck(a1->vendor_idx == 0, "video vendor_idx=%u（期望 0）", a1->vendor_idx);
    ck(a2->vendor_idx == 1, "ctrl vendor_idx=%u（期望 1）", a2->vendor_idx);
    ck(a3->vendor_idx == USB_FN_NO_VENDOR_IDX, "NCM vendor_idx=%u（期望 0xFF）", a3->vendor_idx);
    ck(uc_fn_by_vendor_idx(0) == 1 && uc_fn_by_vendor_idx(1) == 2, "uc_fn_by_vendor_idx 反查错误");
    ck(uc_fn_by_vendor_idx(2) == -1, "uc_fn_by_vendor_idx(2) 应返回 -1");
}

static void check_identity(void)
{
    ctx_set("§3 身份 / 设备描述符");
    usb_composite_identity_t id;
    memset(&id, 0, sizeof(id));
    usb_composite_identity_default(&id);
    ck(id.vid == 0x303A, "idVendor=0x%04X（冻结 §3：0x303A）", id.vid);
    ck(id.pid == 0x4103, "idProduct=0x%04X（冻结 §3：0x4103）", id.pid);
    ck((id.pid < 0x4000 || id.pid > 0x407F),
       "idProduct=0x%04X 落在 esp_tinyusb 自动 PID 空间 0x4000~0x407F 内（必须避开）", id.pid);
    ck(id.bcd_usb >= 0x0210, "bcdUSB=0x%04X（必须 >= 0x0210，否则主机不取 BOS）", id.bcd_usb);
    ck(id.b_device_class == 0xEF && id.b_device_subclass == 0x02 && id.b_device_protocol == 0x01,
       "bDeviceClass/Sub/Proto=0x%02X/%02X/%02X（冻结 §3：EF/02/01）", id.b_device_class,
       id.b_device_subclass, id.b_device_protocol);
    ck(id.max_power_ma <= 500, "max_power_ma=%u（必须 <= 500）", id.max_power_ma);
    if (id.bcd_device != 0x0100) {
        info("bcdDevice=0x%04X（冻结 §3 起始 0x0100；每次改描述符必须 +1，本处只要求 >= 0x0100）",
             id.bcd_device);
    }
    ck(id.bcd_device >= 0x0100, "bcdDevice=0x%04X（冻结 §3：>= 0x0100）", id.bcd_device);

    uint32_t len = 0;
    const uint8_t *d = uc_dev_desc(&len);
    if (!ck(d != NULL && len == 18, "设备描述符长度=%u（期望 18）", len)) {
        return;
    }
    ck(d[0] == 18 && d[1] == DT_DEVICE, "设备描述符 bLength/bDescriptorType=%u/%u（期望 18/1）",
       d[0], d[1]);
    ck(rd16(d + 2) == id.bcd_usb, "描述符 bcdUSB=0x%04X != identity 0x%04X", rd16(d + 2), id.bcd_usb);
    ck(d[4] == id.b_device_class && d[5] == id.b_device_subclass && d[6] == id.b_device_protocol,
       "描述符 类/子类/协议=%02X/%02X/%02X != identity", d[4], d[5], d[6]);
    ck(d[7] == 64, "bMaxPacketSize0=%u（期望 64）", d[7]);
    ck(rd16(d + 8) == id.vid, "idVendor=0x%04X（期望 0x%04X）", rd16(d + 8), id.vid);
    ck(rd16(d + 10) == id.pid, "idProduct=0x%04X（期望 0x%04X）", rd16(d + 10), id.pid);
    ck(rd16(d + 12) == id.bcd_device, "bcdDevice=0x%04X（期望 0x%04X）", rd16(d + 12), id.bcd_device);
    ck(d[14] == 1 && d[15] == 2 && d[16] == 3,
       "iManufacturer/iProduct/iSerialNumber=%u/%u/%u（§4：1/2/3）", d[14], d[15], d[16]);
    ck(d[17] == 1, "bNumConfigurations=%u（期望 1）", d[17]);

    /* HS 口必须有 qualifier（esp_tinyusb 在 HS 口上需要它） */
    uint32_t qlen = 0;
    const uint8_t *q = uc_qual_desc(&qlen);
    ck(q != NULL && qlen == 10, "设备限定符长度=%u（期望 10）", qlen);
    if (q && qlen == 10) {
        ck(q[0] == 10 && q[1] == 0x06, "qualifier bLength/type=%u/0x%02X（期望 10/0x06）", q[0], q[1]);
        ck(q[4] == d[4] && q[5] == d[5] && q[6] == d[6],
           "qualifier 类字段必须与 device 描述符一致（USB 2.0 §9.6.2）");
    }
}

static void check_strings(void)
{
    ctx_set("§4 字符串描述符");
    int count = -1;
    const char *const *s = uc_str_desc(&count);
    if (!ck(s != NULL && count == 8, "字符串条数=%d（冻结 §4：恰好 8 条用满）", count)) {
        return;
    }
    ck(USB_COMPOSITE_MAX_STR == 8 && count <= USB_COMPOSITE_MAX_STR,
       "字符串条数 %d > 上限 %d", count, USB_COMPOSITE_MAX_STR);
    for (int i = 1; i < count; i++) {
        ck(s[i] != NULL && s[i][0] != 0, "字符串[%d] 为空", i);
    }
    if (s[1]) ck(strcmp(s[1], "MeidaP4") == 0, "字符串[1]='%s'（期望 MeidaP4）", s[1]);
    if (s[2]) ck(strcmp(s[2], "MeidaP4 Composite") == 0, "字符串[2]='%s'（期望 MeidaP4 Composite）", s[2]);
    if (s[3]) ck(strlen(s[3]) >= 8, "字符串[3]（序列号）长度=%zu（期望 >= 8）", strlen(s[3]));
    if (s[4]) ck(strcmp(s[4], "MeidaP4 SD Card") == 0, "字符串[4]='%s'（§4 更正后期望 MeidaP4 SD Card）", s[4]);
    if (s[5]) ck(strcmp(s[5], "MeidaP4 WebUSB") == 0, "字符串[5]='%s'（§4 更正后期望 MeidaP4 WebUSB）", s[5]);
    if (s[6]) ck(strcmp(s[6], "MeidaP4 Network") == 0, "字符串[6]='%s'（期望 MeidaP4 Network）", s[6]);
    if (s[7]) {
        ck(strlen(s[7]) == 12, "字符串[7]（NCM MAC）长度=%zu（期望 12 个十六进制字符）", strlen(s[7]));
        for (size_t i = 0; s[7][i]; i++) {
            ck(isxdigit((unsigned char)s[7][i]) != 0, "字符串[7] 第 %zu 个字符 '%c' 不是十六进制", i, s[7][i]);
        }
    }
    info("字符串[0]=%s（LANGID）", s[0] ? "(2 字节 LANGID)" : "(NULL)");
    /* §4 idx 0 = LANGID 0x0409：esp_tinyusb 的字符串表约定是"索引 0 放 2 字节小端 LANGID"
     * （批 0 真机验证过的 build-final-b 里 idx0 = b'\t\x04'，见 desc_lint 的字符串 dump） */
    if (ck(s[0] != NULL, "字符串[0] 为空（§4：idx0 = LANGID 0x0409）")) {
        ck((uint8_t)s[0][0] == 0x09 && (uint8_t)s[0][1] == 0x04,
           "字符串[0] 不是 LANGID 0x0409 的小端字节（实测 %02X %02X）",
           (uint8_t)s[0][0], (uint8_t)s[0][1]);
    }
}

static void check_bos(void)
{
    ctx_set("§5 BOS / WebUSB 平台能力");
    uint32_t len = 0;
    const uint8_t *d = uc_bos_desc(&len);
    if (!ck(d != NULL && len >= 5, "BOS 为空")) {
        return;
    }
    ck(len == EXPECT_BOS_LEN, "BOS 长度=%u（期望 %u：与批 0 真机验证过的 final-b 一致）", len, EXPECT_BOS_LEN);
    ck(d[0] == 5 && d[1] == DT_BOS, "BOS bLength/type=%u/0x%02X（期望 5/0x0F）", d[0], d[1]);
    ck(rd16(d + 2) == len, "BOS wTotalLength=%u != 实际 %u", rd16(d + 2), len);
    ck(d[4] == 2, "BOS bNumDeviceCaps=%u（期望 2：WebUSB + MS OS 2.0）", d[4]);

    /* WebUSB 平台能力：d[5..29)，UUID @9，bVendorCode @27，iLandingPage @28 */
    if (len >= 29) {
        ck(d[5] == 0x18 && d[6] == DT_DEV_CAP && d[7] == 0x05,
           "WebUSB 能力 bLength/type/capType=%u/0x%02X/0x%02X（期望 24/0x10/0x05）", d[5], d[6], d[7]);
        ck(memcmp(d + 9, UUID_WEBUSB, 16) == 0, "WebUSB 平台 UUID 不匹配");
        ck(rd16(d + 25) == 0x0100, "WebUSB bcdVersion=0x%04X（期望 0x0100）", rd16(d + 25));
        ck(d[27] == 0x01, "WebUSB bVendorCode=0x%02X（冻结 §5：WebUSB = 0x01）", d[27]);
        ck(d[28] == 0x00, "WebUSB iLandingPage=%u（冻结 §5：必须为 0）", d[28]);
    } else {
        ck(0, "BOS 太短，装不下 WebUSB 平台能力");
    }

    /* MS OS 2.0 平台能力：d[29..57)，UUID @33，dwWindowsVersion @49，总长 @53，bVendorCode @55 */
    if (len >= 57) {
        ck(d[29] == 0x1C && d[30] == DT_DEV_CAP && d[31] == 0x05,
           "MS OS 能力 bLength/type/capType=%u/0x%02X/0x%02X（期望 28/0x10/0x05）", d[29], d[30], d[31]);
        ck(memcmp(d + 33, UUID_MSOS20, 16) == 0, "MS OS 2.0 平台 UUID 不匹配");
        ck(rd32(d + 49) == 0x06030000u, "MS OS dwWindowsVersion=0x%08X（期望 0x06030000）", rd32(d + 49));
        ck(rd16(d + 53) == EXPECT_MSOS_LEN, "BOS.wMSOSDescriptorSetTotalLength=%u（期望 %u）",
           rd16(d + 53), EXPECT_MSOS_LEN);
        ck(d[55] == 0x02, "MS OS bVendorCode=0x%02X（冻结 §5：MS OS = 0x02）", d[55]);
        ck(d[56] == 0x00, "MS OS bAltEnumCode=%u（期望 0）", d[56]);
    } else {
        ck(0, "BOS 太短，装不下 MS OS 2.0 平台能力");
    }
}

static void check_msos(void)
{
    ctx_set("§5 MS OS 2.0 描述符集");
    uint32_t len = 0;
    const uint8_t *s = uc_msos_desc(&len);
    if (!ck(s != NULL && len > 0, "MS OS 2.0 描述符集为空")) {
        return;
    }
    info("MS OS 2.0 集 %u 字节（期望 %u = 0x0A + %d × (0x08 + 0x14 + 0x80)）",
         len, EXPECT_MSOS_LEN, EXPECT_MSOS_WCID_N);
    ck(len == EXPECT_MSOS_LEN, "MS OS 2.0 集长度=%u（期望 %u：IF1 视频 + IF2 控制各一个 Function Subset）", len,
       EXPECT_MSOS_LEN);
    if (len != EXPECT_MSOS_LEN) {
        return;
    }

    /* --- Set Header --- */
    ck(rd16(s + 0) == 0x000A, "Set Header wLength=%u（期望 0x000A）", rd16(s + 0));
    ck(rd16(s + 2) == 0x0000, "Set Header wDescriptorType=0x%04X（期望 0x0000）", rd16(s + 2));
    ck(rd32(s + 4) == 0x06030000u, "Set Header dwWindowsVersion=0x%08X（期望 0x06030000）", rd32(s + 4));
    ck(rd16(s + 8) == EXPECT_MSOS_LEN, "Set Header wTotalLength=%u（期望 %u）", rd16(s + 8), EXPECT_MSOS_LEN);

    /* --- 逐个 Function Subset（顺序 = 注册顺序：IF1 视频，IF2 控制） --- */
    static const uint8_t expect_first[EXPECT_MSOS_WCID_N] = { 1, 2 };
    size_t off = 0x0A;
    for (int i = 0; i < EXPECT_MSOS_WCID_N; i++) {
        ctx_set("§5 MS OS 2.0 Function Subset #%d", i + 1);
        const size_t su = off;
        ck(rd16(s + su + 0) == 0x0008, "Function Subset wLength=%u（期望 8）", rd16(s + su + 0));
        ck(rd16(s + su + 2) == 0x0002, "Function Subset wDescriptorType=0x%04X（期望 0x0002）",
           rd16(s + su + 2));
        ck(s[su + 4] == expect_first[i],
           "Function Subset bFirstInterfaceNumber=%u（期望 %u：T2 起 IF1/IF2 都要 WinUSB）",
           s[su + 4], expect_first[i]);
        ck(s[su + 5] == 0, "Function Subset bReserved=%u（期望 0）", s[su + 5]);
        ck(rd16(s + su + 6) == EXPECT_MSOS_SUBSET_LEN, "Function Subset wSubsetLength=%u（期望 %u）",
           rd16(s + su + 6), EXPECT_MSOS_SUBSET_LEN);

        /* --- CompatibleID --- */
        const size_t co = su + 8;
        ck(rd16(s + co + 0) == 0x0014, "CompatibleID wLength=%u（期望 0x14）", rd16(s + co + 0));
        ck(rd16(s + co + 2) == 0x0003, "CompatibleID wDescriptorType=0x%04X（期望 0x0003）", rd16(s + co + 2));
        ck(memcmp(s + co + 4, "WINUSB\0\0", 8) == 0, "CompatibleID 不是 WINUSB\\0\\0");
        ck(memcmp(s + co + 12, "\0\0\0\0\0\0\0\0", 8) == 0, "SubCompatibleID 不是 8×0x00");

        /* --- Registry Property（§5：单数 DeviceInterfaceGUID + REG_SZ） --- */
        const size_t rp = co + 0x14;
        ck(rd16(s + rp + 0) == 0x0080, "RegProp wLength=%u（期望 0x80 = 128）", rd16(s + rp + 0));
        ck(rd16(s + rp + 2) == 0x0004, "RegProp wDescriptorType=0x%04X（期望 0x0004）", rd16(s + rp + 2));
        ck(rd16(s + rp + 4) == 0x0001, "RegProp wPropertyDataType=0x%04X（冻结 §5：必须 0x0001 = REG_SZ）",
           rd16(s + rp + 4));
        ck(rd16(s + rp + 6) == 40, "RegProp wPropertyNameLength=%u（期望 40 = (19+1)×2）", rd16(s + rp + 6));
        {
            uint8_t exp_name[64];
            size_t n = utf16le("DeviceInterfaceGUID", exp_name, sizeof(exp_name));
            ck(n == 40, "期望的 UTF-16 名字长度=%zu（期望 40）", n);
            ck(memcmp(s + rp + 8, exp_name, 40) == 0,
               "PropertyName 不是单数 DeviceInterfaceGUID（UTF-16LE）");
            ck(s[rp + 8 + 38] == 0x00 && s[rp + 8 + 39] == 0x00,
               "PropertyName 结尾疑似复数形式 DeviceInterfaceGUIDs（偏移 +38 应为 0x00）");
        }
        ck(rd16(s + rp + 48) == 78, "RegProp wPropertyDataLength=%u（期望 78 = (38+1)×2）", rd16(s + rp + 48));
        {
            uint8_t exp_data[128];
            size_t n = utf16le(GUID_USB_DEVICE, exp_data, sizeof(exp_data));
            ck(n == 78, "期望的 GUID UTF-16 长度=%zu（期望 78）", n);
            ck(memcmp(s + rp + 50, exp_data, 78) == 0,
               "PropertyData 不等于 %s 的 UTF-16LE（含结尾 NUL）", GUID_USB_DEVICE);
        }
        off = rp + 0x80;
    }

    ctx_set("§5 MS OS 2.0 描述符集");
    ck(off == len, "子描述符累加 %zu != 集长度 %u（RegProp 必须正好收尾）", off, len);

    /* 全集合里不得出现 Configuration Subset（wDescriptorType 0x0001） */
    {
        int cfg_subset = 0;
        size_t o = 0x0A;
        while (o + 4 <= len) {
            const size_t wlen = rd16(s + o + 0);
            const uint16_t wtype = rd16(s + o + 2);
            if (wlen < 4 || o + wlen > len) {
                break;
            }
            if (wtype == 0x0001) {
                cfg_subset++;
            }
            o += wlen;
        }
        ck(cfg_subset == 0, "发现 Configuration Subset Header (0x0001)：冻结 §5.2 默认必须关（与真机验证形态一致）");
    }
    /* 不得出现 DeviceInterfaceGUIDs（复数，真机 Code 31 的元凶） */
    {
        const char *plural = "DeviceInterfaceGUIDs";
        uint8_t needle[64];
        size_t n = utf16le(plural, needle, sizeof(needle));
        int found = 0;
        for (size_t i = 0; i + n <= len; i++) {
            if (memcmp(s + i, needle, n) == 0) {
                found = 1;
                break;
            }
        }
        ck(found == 0, "MS OS 集合里出现了复数 DeviceInterfaceGUIDs（真机实测会导致 winusb.sys Code 31）");
    }
}

static void check_budget(void)
{
    ctx_set("§2.2 DWC2 FIFO 预算 / 预算汇总");
    const usb_composite_budget_t *b = uc_budget();
    if (!ck(b != NULL, "uc_budget() 返回空")) {
        return;
    }
    info("预算: itf=%u fn=%u vendor=%u IN=%u OUT=%u FIFO=%u/%u(余 %u) FS=%u HS=%u BOS=%u MSOS=%u STR=%u",
         b->itf_total, b->fn_total, b->vendor_fn_total, b->ep_in_total, b->ep_out_total,
         b->fifo_words_used, b->fifo_words_total, b->fifo_words_free, b->fs_cfg_len, b->hs_cfg_len,
         b->bos_len, b->msos_len, b->str_count);

    ck(b->itf_total == 5, "itf_total=%u（冻结 §1：5）", b->itf_total);
    ck(b->fn_total == 4, "fn_total=%u（4 个功能）", b->fn_total);
    ck(b->vendor_fn_total == 2, "vendor_fn_total=%u（IF1+IF2 = 2 个 vendor 功能）", b->vendor_fn_total);
    ck(b->ep_in_total == 6, "ep_in_total=%u（冻结 §2：6）", b->ep_in_total);
    ck(b->ep_in_total <= 7, "ep_in_total=%u 超过 7", b->ep_in_total);
    ck(b->ep_out_total == 3, "ep_out_total=%u（冻结 §2：3）", b->ep_out_total);
    ck(b->str_count == 8, "str_count=%u（冻结 §4：8）", b->str_count);
    ck(b->bos_len == EXPECT_BOS_LEN, "bos_len=%u（期望 %u）", b->bos_len, EXPECT_BOS_LEN);
    ck(b->msos_len == EXPECT_MSOS_LEN, "msos_len=%u（期望 %u）", b->msos_len, EXPECT_MSOS_LEN);

    /* §2.2 独立复算：EP0 16 + GRXFSIZ 304 + EP1 16 + EP2 128 + EP3 128 + EP4 128 + EP5 4 + EP6 128 */
    ck(b->fifo_words_total == EXPECT_FIFO_WORDS_TOTAL, "fifo_words_total=%u（期望 %u）",
       b->fifo_words_total, EXPECT_FIFO_WORDS_TOTAL);
    ck(b->fifo_words_used == EXPECT_FIFO_WORDS_USED,
       "fifo_words_used=%u（按冻结 §2.2 实算应为 %u）", b->fifo_words_used, EXPECT_FIFO_WORDS_USED);
    ck(b->fifo_words_used <= b->fifo_words_total, "FIFO 预算超限：%u > %u", b->fifo_words_used,
       b->fifo_words_total);
    ck(b->fifo_words_free == EXPECT_FIFO_WORDS_FREE, "fifo_words_free=%u（独立复算应为 %u）",
       b->fifo_words_free, EXPECT_FIFO_WORDS_FREE);
    ck(b->fs_cfg_len > 0 && b->hs_cfg_len > 0, "FS/HS 配置描述符长度必须 > 0");
    ck(b->fs_cfg_len == b->hs_cfg_len, "FS(%u) 与 HS(%u) 配置描述符长度应相同（只有 mps 不同）",
       b->fs_cfg_len, b->hs_cfg_len);
}

static void check_api_contract(void)
{
    ctx_set("API 契约：幂等 / 顺序 / 自检");
    esp_err_t r1 = uc_prepare();
    ck(r1 == ESP_OK, "重复调用 uc_prepare() 应返回 ESP_OK（幂等），实测 0x%X", r1);
    ck(uc_is_prepared(), "uc_is_prepared() 应为 true");

    const usb_fn_alloc_t *a_before = uc_fn_alloc(0);
    uint8_t snap[16];
    memcpy(snap, a_before, sizeof(snap));
    uc_prepare();
    ck(memcmp(snap, uc_fn_alloc(0), sizeof(snap)) == 0, "重复 prepare 改变了分配结果（必须幂等）");

    esp_err_t r2 = uc_register(&FN_MTP);
    ck(r2 == ESP_ERR_INVALID_STATE, "prepare 之后 uc_register() 应返回 ESP_ERR_INVALID_STATE，实测 0x%X", r2);

    uint32_t run = 0, failed = 0;
    esp_err_t r3 = uc_selfcheck(&run, &failed);
    ck(run > 0, "uc_selfcheck() 执行的检查条数=%u（应 > 0）", run);
    ck(failed == 0, "uc_selfcheck() 失败条数=%u（应 0）：%s", failed, uc_last_error());
    ck(r3 == ESP_OK, "uc_selfcheck() 应返回 ESP_OK，实测 0x%X（%s）", r3, uc_last_error());
    if (run > 0) {
        info("uc_selfcheck(): %u 条检查全部通过", run);
    }
}

/* =====================================================================
 * 负例路径：超预算 / 非法计数必须被拒绝
 * ===================================================================== */

static esp_err_t bad_build(bool hs, const usb_fn_alloc_t *a, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t o = 0;
    o += emit_iface(buf + o, a->itf[0], 0, a->ep_in_count, USBCFG_ITF_CLASS_VENDOR, 0, 0, 0);
    for (int i = 0; i < a->ep_in_count; i++) {
        o += emit_ep(buf + o, a->ep_in[i], XFER_BULK, hs ? 512 : 64, 0);
    }
    (void)cap;
    *out_len = o;
    return ESP_OK;
}

static int run_over_budget(void)
{
    printf("==============================================================================\n");
    printf("MeidaP4 · I1 负例路径：超预算/非法端点计数必须被拒绝\n");
    printf("==============================================================================\n");

    ctx_set("负例 A：ep_in_count=8（超出 MAX_EP_PER_DIR=7）注册必须被拒");
    {
        usb_function_t f = {
            .name = "over-count",
            .itf_count = 1,
            .ep_in_count = 8,                 /* 数组只有 7 项：实现必须先校验上界 */
            .ep_out_count = 0,
            .build_desc = bad_build,
        };
        for (int i = 0; i < USB_COMPOSITE_MAX_EP_PER_DIR; i++) {
            f.ep_in_req[i].xfer = USB_EP_XFER_BULK;
            f.ep_in_req[i].mps_hs = 512;
            f.ep_in_req[i].mps_fs = 64;
        }
        esp_err_t r = uc_register(&f);
        ck(r != ESP_OK, "ep_in_count=8 的注册被接受（应返回 ESP_ERR_INVALID_ARG），实测 0x%X", r);
    }

    /* 重新用一个干净进程状态做负例 B：这里注册 4 个正常功能 + 1 个合法计数但超预算的功能 */
    ctx_set("负例 B：合法计数（7 个 bulk IN）但总 IN = 13 > 7，prepare 必须失败");
    {
        esp_err_t r = ESP_OK;
        r |= uc_register(&FN_MTP);
        r |= uc_register(&FN_VIDEO);
        r |= uc_register(&FN_CTRL);
#ifdef USE_REAL_NCM
        r |= usb_net_ncm_register();
#else
        r |= uc_register(&FN_NCM);
#endif
        usb_function_t f = {
            .name = "too-many-in",
            .itf_count = 1,
            .ep_in_count = 7,
            .ep_out_count = 0,
            .build_desc = bad_build,
        };
        for (int i = 0; i < 7; i++) {
            f.ep_in_req[i].xfer = USB_EP_XFER_BULK;
            f.ep_in_req[i].mps_hs = 512;
            f.ep_in_req[i].mps_fs = 64;
        }
        r |= uc_register(&f);
        esp_err_t p = uc_prepare();
        ck(p != ESP_OK, "总 IN = 6+7 = 13 > 7 时 uc_prepare() 返回了 ESP_OK —— 预算检查缺失");
        if (p != ESP_OK) {
            info("uc_prepare() 正确拒绝：0x%X（%s）", p, uc_last_error());
        }
    }

    printf("------------------------------------------------------------------------------\n");
    printf("断言汇总: %d 条，通过 %d，失败 %d\n", g_pass + g_fail, g_pass, g_fail);
    printf("判定    : %s\n", g_fail == 0 ? "PASS（负例都被正确拒绝）" : "FAIL（负例未被拒绝 —— 预算检查有缺口）");
    printf("==============================================================================\n");
    return g_fail == 0 ? 0 : 1;
}

/* =====================================================================
 * main
 * ===================================================================== */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--over-budget") == 0) {
        return run_over_budget();
    }

    printf("==============================================================================\n");
    printf("MeidaP4 · I1 复合层离线验证（frozen-allocation.md 逐行）\n");
    printf("  被验证 : components/usb_composite/usb_composite_desc.c\n");
    printf("  期望值 : docs/contracts/frozen-allocation.md 逐行手抄\n");
#ifdef USE_REAL_NCM
    printf("  原料   : 3 个 mock（MTP/video/ctrl）+ **WS-net 真实 NCM 片段**（usb_net_ncm_register）\n");
#else
    printf("  原料   : 4 个 mock usb_function_t（不硬编码任何接口号/端点号）\n");
#endif
    printf("==============================================================================\n");

    /* ---- 注册（顺序 = 冻结 §1 的接口顺序） ---- */
    {
        esp_err_t r = uc_register(&FN_MTP);
        ck(r == ESP_OK, "uc_register(MTP) = 0x%X（%s）", r, uc_last_error());
        r = uc_register(&FN_VIDEO);
        ck(r == ESP_OK, "uc_register(video) = 0x%X（%s）", r, uc_last_error());
        r = uc_register(&FN_CTRL);
        ck(r == ESP_OK, "uc_register(ctrl) = 0x%X（%s）", r, uc_last_error());
#ifdef USE_REAL_NCM
        /* 集成模式：注册 WS-net 的**真实** NCM 片段（不经过本文件的 mock） */
        r = usb_net_ncm_register();
        ck(r == ESP_OK, "usb_net_ncm_register()（真实片段）= 0x%X（%s）", r, uc_last_error());
#else
        r = uc_register(&FN_NCM);
        ck(r == ESP_OK, "uc_register(NCM) = 0x%X（%s）", r, uc_last_error());
#endif
        ck(uc_fn_count() == 4, "注册后功能数=%d（期望 4）", uc_fn_count());
    }
    {
        esp_err_t p = uc_prepare();
        ck(p == ESP_OK, "uc_prepare() = 0x%X（%s）", p, uc_last_error());
        if (p != ESP_OK) {
#ifdef USE_REAL_NCM
            printf("------------------------------------------------------------------------------\n");
            printf("提示（集成模式）：真实 NCM 片段注册失败时请先排查**结构体生命周期** ——\n");
            printf("  usb_composite_desc.c:260 `s.fn[s.fn_count] = fn;` 保存的是**调用方指针**，\n");
            printf("  而 usb_net_ncm.c:255 用的是栈上局部 `usb_function_t fn;`（返回即失效）⇒\n");
            printf("  prepare 阶段读到的是被覆写的栈内存（UB）。\n");
            printf("  对照：usb_vendor 的两个功能是 `const usb_function_t` 全局，mtp_backend 是 static ⇒ 不受影响。\n");
#endif
            printf("------------------------------------------------------------------------------\n");
            printf("断言汇总: %d 条，通过 %d，失败 %d\n", g_pass + g_fail, g_pass, g_fail);
            printf("判定    : FAIL（prepare 失败，无法继续核对冻结表）\n");
            printf("==============================================================================\n");
            return 1;
        }
    }

    check_alloc_table();
    check_identity();
    check_strings();
    check_config("FS (Full Speed)", false);
    check_config("HS (High Speed)", true);
    check_bos();
    check_msos();
    check_budget();
    check_api_contract();

    printf("------------------------------------------------------------------------------\n");
    printf("断言汇总: %d 条，通过 %d，失败 %d\n", g_pass + g_fail, g_pass, g_fail);
    printf("判定    : %s\n", g_fail == 0
           ? "PASS（描述符/端点/字符串/BOS/MS OS/IAD/FIFO 与 frozen-allocation.md 一致）"
           : "FAIL（与冻结表不符，见上面逐条 FAIL）");
    printf("边界    : 本结果**只**证明装配器输出的字节与冻结表一致；\n");
    printf("          **不能**证明真机枚举、Windows 驱动绑定、FIFO 实际分配成立。设备未连接。\n");
    printf("==============================================================================\n");
    return g_fail == 0 ? 0 : 1;
}
