// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · NCM 描述符片段 + 通过 I1（usb_composite）注册
//
// 冻结依据：docs/contracts/frozen-allocation.md
//   §1  IF3 = NCM 控制 `02/0D/00`（CDC / NCM）、IF4 = NCM 数据 `0A/00/01`（CDC-Data）
//   §2  EP5 = int IN 16B interval 8（通知）、EP6 = bulk IN/OUT 512(HS)/64(FS)（数据）
//   §6  NCM（IF3+IF4）**必须带 IAD**：bFirstInterface=3、bInterfaceCount=2、
//       bFunctionClass/SubClass/Protocol = 0x02/0x0D/0x00
//
// ⚠️ 本文件**不构造 IAD** —— I1 在 `usb_function_t.use_iad = true` 时会自己插 8 字节 IAD，
//    并且把它的类字段**抄自本片段第一个接口描述符**（usb_composite_desc.c:518-531）。
//    所以本片段必须**从 CDC 控制接口描述符开始**，且该接口的 class/subclass/protocol
//    必须是 0x02/0x0D/0x00。
//
// ⚠️ 接口号/端点号/字符串索引**全部来自 alloc**，本文件没有任何硬编码号。
//
// 逐字节来源：tinyusb `src/device/usbd.h:1080-1102` 的 `TUD_CDC_NCM_DESCRIPTOR_arg11()`
// **去掉它的 IAD 那 8 字节**（其余 77 字节与本文件一致）。之所以不直接用那个宏：
//   1) 它会自己带 IAD → 与 I1 插入的 IAD 重复；
//   2) 它是初始化列表宏，跑的是一组编译期常量，而这里接口号/端点号是运行期分配的。
// 本文件的字节表在 `test/host/test_ncm_desc.c` 里被本地 gcc 逐字节复算过（离线自检）。

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "usb_composite.h"
#include "usb_net.h"
#include "usb_net_priv.h"

static const char *TAG = "usb_net_ncm";

/* ---------------- USB 描述符常量（USB 2.0 §9.6 / CDC 1.2 / CDC-NCM 1.0） ---------------- */
#define DT_INTERFACE        0x04
#define DT_ENDPOINT         0x05
#define DT_CS_INTERFACE     0x24
#define XFER_INTERRUPT      0x03
#define XFER_BULK           0x02

#define CDC_CLASS           0x02
#define CDC_SUBCLASS_NCM    0x0D
#define CDC_DATA_CLASS      0x0A
#define CDC_DATA_PROTO_NTB  0x01        /* NCM 数据接口的协议码：Network Transfer Block
                                          （tinyusb net_device.h:47 NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK） */
#define CDC_FD_HEADER       0x00
#define CDC_FD_UNION        0x06
#define CDC_FD_ETH_NETWORK  0x0F
#define CDC_FD_NCM          0x1A

#define NCM_BCD_CDC         0x0110
#define NCM_BCD_NCM         0x0100
/* bmNetworkCapabilities = ETH_FILTER(bit0) | NTB_INPUT_SIZE(bit5) —— 与 tinyusb 宏的默认值一致 */
#define NCM_CAPS            0x21

/* 片段长度：8(IAD, 由 I1 插) + 本文件 77 = 85 = tinyusb TUD_CDC_NCM_DESC_LEN */
#define NCM_FRAG_LEN_WITHOUT_IAD  (9 + 5 + 5 + 13 + 6 + 7 + 9 + 9 + 7 + 7)

/* ---------------- 极小的字节写出器（little-endian） ---------------- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
} bd_t;

static void bd_u8(bd_t *b, uint8_t v)
{
    if (b->len < b->cap) {
        b->buf[b->len] = v;
    } else {
        b->overflow = true;
    }
    b->len++;
}

static void bd_u16(bd_t *b, uint16_t v)
{
    bd_u8(b, (uint8_t)(v & 0xFF));
    bd_u8(b, (uint8_t)(v >> 8));
}

static void bd_itf(bd_t *b, uint8_t num, uint8_t alt, uint8_t n_ep,
                   uint8_t cls, uint8_t sub, uint8_t proto, uint8_t str_idx)
{
    bd_u8(b, 9);
    bd_u8(b, DT_INTERFACE);
    bd_u8(b, num);
    bd_u8(b, alt);
    bd_u8(b, n_ep);
    bd_u8(b, cls);
    bd_u8(b, sub);
    bd_u8(b, proto);
    bd_u8(b, str_idx);
}

static void bd_ep(bd_t *b, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval)
{
    bd_u8(b, 7);
    bd_u8(b, DT_ENDPOINT);
    bd_u8(b, addr);
    bd_u8(b, attr);
    bd_u16(b, mps);
    bd_u8(b, interval);
}

/* ---------------- 片段生成 ---------------- */

static esp_err_t ncm_build_desc(bool high_speed, const usb_fn_alloc_t *a,
                                uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!a || !buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (a->itf_count < 2 || a->ep_in_count < 2 || a->ep_out_count < 1) {
        ESP_LOGE(TAG, "alloc 不足：itf=%u ep_in=%u ep_out=%u（NCM 需要 2 接口 + 2 IN + 1 OUT）",
                 (unsigned)a->itf_count, (unsigned)a->ep_in_count, (unsigned)a->ep_out_count);
        return ESP_ERR_INVALID_STATE;
    }
    if (cap < NCM_FRAG_LEN_WITHOUT_IAD) {
        ESP_LOGE(TAG, "描述符缓冲不足：cap=%u 需要 %u", (unsigned)cap, (unsigned)NCM_FRAG_LEN_WITHOUT_IAD);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t itf_ctrl = a->itf[0];              /* IF3：NCM 控制 */
    const uint8_t itf_data = a->itf[1];              /* IF4：NCM 数据 */
    const uint8_t ep_notif = a->ep_in[0];            /* EP5：int IN */
    const uint8_t ep_data_in = a->ep_in[1];          /* EP6：bulk IN */
    const uint8_t ep_data_out = a->ep_out[0];        /* EP6：bulk OUT（由 I1 配对成同号） */
    const uint8_t str_itf = a->itf_str_idx;          /* "MeidaP4 Network" */
    const uint8_t str_mac = a->extra_str_idx;        /* "AABBCCDDEEFF" */
    const uint16_t mps_bulk = high_speed ? USB_NET_NCM_BULK_MPS_HS : USB_NET_NCM_BULK_MPS_FS;

    bd_t b = { .buf = buf, .cap = cap, .len = 0, .overflow = false };

    /* [1] CDC 控制接口（IAD 的类字段由 I1 抄这一条） */
    bd_itf(&b, itf_ctrl, 0, 1, CDC_CLASS, CDC_SUBCLASS_NCM, 0x00, str_itf);

    /* [2] Header Functional Descriptor：bcdCDC = 0x0110 */
    bd_u8(&b, 5);
    bd_u8(&b, DT_CS_INTERFACE);
    bd_u8(&b, CDC_FD_HEADER);
    bd_u16(&b, NCM_BCD_CDC);

    /* [3] Union Functional Descriptor：master = 控制接口，slave = 数据接口 */
    bd_u8(&b, 5);
    bd_u8(&b, DT_CS_INTERFACE);
    bd_u8(&b, CDC_FD_UNION);
    bd_u8(&b, itf_ctrl);
    bd_u8(&b, itf_data);

    /* [4] Ethernet Networking Functional Descriptor（13 B）
     *     iMACAddress, bmEthernetStatistics(4), wMaxSegmentSize, wNumberMCFilters(2), bNumberPowerFilters */
    bd_u8(&b, 13);
    bd_u8(&b, DT_CS_INTERFACE);
    bd_u8(&b, CDC_FD_ETH_NETWORK);
    bd_u8(&b, str_mac);
    bd_u8(&b, 0);
    bd_u8(&b, 0);
    bd_u8(&b, 0);
    bd_u8(&b, 0);
    bd_u16(&b, (uint16_t)g_usbnet.seg_size);
    bd_u16(&b, 0);
    bd_u8(&b, 0);

    /* [5] NCM Functional Descriptor（6 B）：bcdNCM = 0x0100，bmNetworkCapabilities */
    bd_u8(&b, 6);
    bd_u8(&b, DT_CS_INTERFACE);
    bd_u8(&b, CDC_FD_NCM);
    bd_u16(&b, NCM_BCD_NCM);
    bd_u8(&b, NCM_CAPS);

    /* [6] 通知端点（interrupt IN，16 B / interval 8ms —— 冻结表 §2） */
    bd_ep(&b, ep_notif, XFER_INTERRUPT, USB_NET_NCM_NOTIF_MPS, USB_NET_NCM_NOTIF_INTERVAL);

    /* [7][8] 数据接口两套 alternate setting：
     *     alt0 = 0 个端点（默认、不传数据），alt1 = 2 个 bulk 端点。
     *     Windows 收件箱 usbncm.sys 与 CDC-NCM 1.0 都要求这个形状；
     *     tinyusb 的 ncm_device.c:928-941 会跳过任意个 CDC_DATA 接口再开那 2 个 bulk 端点。 */
    bd_itf(&b, itf_data, 0, 0, CDC_DATA_CLASS, 0x00, CDC_DATA_PROTO_NTB, 0x00);
    bd_itf(&b, itf_data, 1, 2, CDC_DATA_CLASS, 0x00, CDC_DATA_PROTO_NTB, 0x00);

    /* [9][10] 数据端点（先 IN 后 OUT —— 与 tinyusb 宏一致；usbd_open_edpt_pair 两个方向都认） */
    bd_ep(&b, ep_data_in, XFER_BULK, mps_bulk, 0);
    bd_ep(&b, ep_data_out, XFER_BULK, mps_bulk, 0);

    if (b.overflow) {
        ESP_LOGE(TAG, "描述符写出越界（cap=%u）", (unsigned)cap);
        return ESP_ERR_INVALID_SIZE;
    }
    if (b.len != NCM_FRAG_LEN_WITHOUT_IAD) {
        /* 长度表被改坏的兜底（也保护 bd_* 的漏写） */
        ESP_LOGE(TAG, "片段长度 %u != 预期 %u", (unsigned)b.len, (unsigned)NCM_FRAG_LEN_WITHOUT_IAD);
        return ESP_ERR_INVALID_STATE;
    }

    *out_len = b.len;
    return ESP_OK;
}

/* ---------------- 号码校验（I1 分配完之后） ---------------- */

static esp_err_t ncm_init(const usb_fn_alloc_t *a)
{
    if (!a) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 冻结表逐项核对。**只报警不返回错误**：号不对时整个复合设备的描述符仍然自洽
     * （I1 的 selfcheck 也会报），不该因为 NCM 一家把 MTP/vendor 一起拖死 —— 那是
     * "上电后什么都测不了"的最坏结果。 */
    bool ok = true;
    if (a->itf_count != 2 || a->itf[0] != 3 || a->itf[1] != 4) {
        ESP_LOGE(TAG, "❌ NCM 接口号 = %u/%u，冻结表要求 3/4（注册顺序错了？NCM 必须**最后**注册）",
                 a->itf_count > 0 ? a->itf[0] : 0xFF, a->itf_count > 1 ? a->itf[1] : 0xFF);
        ok = false;
    }
    if (a->ep_in_count != 2 || (a->ep_in[0] & 0x7F) != 5 || (a->ep_in[1] & 0x7F) != 6) {
        ESP_LOGE(TAG, "❌ NCM IN 端点 = 0x%02X/0x%02X，冻结表要求 EP5(int)/EP6(bulk)",
                 a->ep_in_count > 0 ? a->ep_in[0] : 0, a->ep_in_count > 1 ? a->ep_in[1] : 0);
        ok = false;
    }
    if (a->ep_out_count != 1 || a->ep_out[0] != 6) {
        ESP_LOGE(TAG, "❌ NCM OUT 端点 = %u，冻结表要求 EP6（与 IN[1] 配对）",
                 a->ep_out_count > 0 ? a->ep_out[0] : 0xFF);
        ok = false;
    }
    if (a->extra_str_idx == 0) {
        ESP_LOGE(TAG, "❌ NCM 的 iMACAddress 字符串索引为 0（extra_string 没进字符串表）");
        ok = false;
    }

    if (ok) {
        ESP_LOGI(TAG, "NCM 分配核对通过：IF%u(ctrl)/IF%u(data)  EP 0x%02X(int,%ums) 0x%02X/0x%02X(bulk)"
                      "  str itf=%u mac=%u  seg=%u",
                 a->itf[0], a->itf[1], a->ep_in[0], USB_NET_NCM_NOTIF_INTERVAL,
                 a->ep_in[1], a->ep_out[0], a->itf_str_idx, a->extra_str_idx,
                 (unsigned)g_usbnet.seg_size);
    }
    return ESP_OK;
}

/* ---------------- 注册 ---------------- */

const char *usb_net_ncm_mac_str(void)
{
    return g_usbnet.mac_str;
}

esp_err_t usb_net_ncm_register(void)
{
    /* ⚠️⚠️ **必须是 static**（不能用栈上局部）—— 这是 verifier 的集成测试当场抓到的真缺陷：
     *   `usb_composite_register()` 只存**指针**（usb_composite_desc.c:260 `s.fn[s.fn_count] = fn;`），
     *   不在注册时复制结构体。栈上局部对象在 `usb_net_ncm_register()` 返回后就失效，
     *   而 `uc_prepare()` 在**另一层调用栈**上解引用它 → 读栈复用后的垃圾：
     *     实测症状 `uc_prepare() = 0x106（NCM: IN 端点号耗尽）` —— 字段被覆写；
     *     更糟时 `fn->build_desc`（函数指针）被覆写 → **跳转到随机地址**。
     *   加 static 后本对象与进程同寿，注册进去的所有指针（含字符串）都长期有效。
     *
     *   注意：即使将来 I1 改成"注册时深拷贝结构体"（verifier 建议方案 A），这里保持 static 也无害。
     *   同等风险点：itf_string/extra_string 必须指向**长期有效**的内存 —— 字符串字面量与
     *   g_usbnet.mac_str（全局单例的字段）都满足，不要改成局部 char buf[]。 */
    static usb_function_t fn;

    memset(&fn, 0, sizeof(fn));

    fn.name = "NCM";
    fn.itf_count = 2;
    /* 字符串：冻结表 §4 的 idx 6（接口）+ idx 7（MAC）。索引由 I1 分配，这里只给内容。
     * extra_string 必须是 12 个十六进制字符（iMACAddress 是字符串描述符索引，内容是 MAC 的
     * 12 个 ASCII 十六进制字符），见 usb_net_ncm_mac_str() 的格式化。 */
    fn.itf_string = "MeidaP4 Network";
    fn.extra_string = usb_net_ncm_mac_str();
    fn.use_iad = true;                  /* IF3+IF4 组成一个功能 → 必须 IAD（冻结表 §6） */

    /* 端点需求：IN[0]=int 通知（EP5）、IN[1]=bulk 数据（EP6）；OUT[0] 复用 IN[1] 的号（EP6） */
    fn.ep_in_req[0].xfer     = USB_EP_XFER_INTERRUPT;
    fn.ep_in_req[0].mps_hs   = USB_NET_NCM_NOTIF_MPS;
    fn.ep_in_req[0].mps_fs   = USB_NET_NCM_NOTIF_MPS;
    fn.ep_in_req[0].interval = USB_NET_NCM_NOTIF_INTERVAL;
    fn.ep_in_req[1].xfer     = USB_EP_XFER_BULK;
    fn.ep_in_req[1].mps_hs   = USB_NET_NCM_BULK_MPS_HS;
    fn.ep_in_req[1].mps_fs   = USB_NET_NCM_BULK_MPS_FS;
    fn.ep_in_req[1].interval = 0;
    fn.ep_in_count = 2;

    fn.ep_out_req[0].xfer     = USB_EP_XFER_BULK;
    fn.ep_out_req[0].mps_hs   = USB_NET_NCM_BULK_MPS_HS;
    fn.ep_out_req[0].mps_fs   = USB_NET_NCM_BULK_MPS_FS;
    fn.ep_out_req[0].interval = 0;
    fn.ep_out_count = 1;

    /* ⚠️ 整表初始化成 USB_EP_NO_PAIR 再填有效项：未列出的元素默认 0 会被当成"复用 IN[0]" */
    for (int i = 0; i < USB_COMPOSITE_MAX_EP_PER_DIR; i++) {
        fn.ep_out_same_num_as_in[i] = USB_EP_NO_PAIR;
    }
    fn.ep_out_same_num_as_in[0] = 1;    /* OUT[0] 用 ep_in_req[1] 分配到的号 → EP6 */

    fn.is_vendor  = false;
    fn.build_desc = ncm_build_desc;
    fn.init       = ncm_init;
    /* ⚠️ on_out 必须留 NULL：NCM 的 OUT 端点由 TinyUSB 的 net 类驱动自己收
     *    （tud_network_recv_cb → 我们在 tinyusb_net 里注册的回调），**不经过 I1 的数据面**。
     *    在这里再注册一个 on_out 会让复合层去 ep_read 抢同一对端点。 */
    fn.on_out     = NULL;
    fn.on_mount   = NULL;
    fn.on_control = NULL;
    fn.user_ctx   = NULL;

    /* NCM 不要 WCID：Windows 用收件箱 usbncm.sys（不像 vendor 接口需要 winusb.sys） */
    fn.wcid = false;
    fn.wcid_compatible_id = NULL;
    fn.wcid_device_interface_guid = NULL;

    esp_err_t err = usb_composite_register(&fn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_composite_register(NCM) 失败: %s", esp_err_to_name(err));
    }
    return err;
}
