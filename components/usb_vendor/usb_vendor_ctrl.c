// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · IF2 · **Vendor 控制**（bulk IN + bulk OUT，EP4；**唯一带 WCID 的功能**）
//
// 分配：docs/contracts/frozen-allocation.md §1/§2/§5
//   IF2 = FF/00/00；EP4 bulk IN/OUT；HS 512 / FS 64；`ep_out_same_num_as_in = { 0 }` ⇒ OUT 复用 IN[0] 的号；
//   `wcid = true` + `CompatibleID = "WINUSB"` + `DeviceInterfaceGUID = {A5DCBF10-...}`（Chrome 只认这个）。
// ⚠️ 接口号/端点号一律取自 `usb_fn_alloc_t`；本文件不硬编码任何 IF/EP 号。
//
// 数据面分工：
//   本文件只做「OUT 有事 → 交给 webusb_proto」「mount → 通知 webusb_proto」「init → 把端点号告诉它」。
//   分帧/组帧/窗口/重传全在 components/webusb_proto（I0 设备侧服务）。

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "usb_composite.h"

#include "usb_vendor_priv.h"
#include "usb_vendor.h"
#include "webusb_proto.h"

static const char *TAG = "uv_ctrl";

static uint8_t s_ep_in;
static uint8_t s_ep_out;
static bool    s_bound;

/* =========================================================================
 * 描述符片段（1 个接口 + 1 IN + 1 OUT）
 * ========================================================================= */

static esp_err_t ctrl_build_desc(bool high_speed, const usb_fn_alloc_t *alloc,
                                 uint8_t *buf, size_t cap, size_t *out_len)
{
    uint16_t mps = high_speed ? UV_BULK_MPS_HS : UV_BULK_MPS_FS;
    size_t need = USB_ITF_DESC_LEN + 2 * USB_EP_DESC_LEN;
    size_t n = 0;

    if (alloc == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (alloc->itf_count < 1 || alloc->ep_in_count < 1 || alloc->ep_out_count < 1) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cap < need) {
        return ESP_ERR_INVALID_SIZE;
    }
    n += uv_itf_desc(buf + n, alloc->itf[0], 2, alloc->itf_str_idx);
    n += uv_ep_desc(buf + n, alloc->ep_in[0], mps, 0);    /* bEndpointAddress 含 0x80（IN） */
    n += uv_ep_desc(buf + n, alloc->ep_out[0], mps, 0);   /* OUT：不含方向位 */
    *out_len = n;
    return ESP_OK;
}

/* =========================================================================
 * 回调
 * ========================================================================= */

static esp_err_t ctrl_init(const usb_fn_alloc_t *alloc)
{
    if (alloc == NULL || alloc->itf_count != 1 || alloc->ep_in_count != 1 || alloc->ep_out_count != 1) {
        ESP_LOGE(TAG, "I1 分配异常（控制功能固定 1 接口 / 1 IN / 1 OUT）");
        return ESP_ERR_INVALID_STATE;
    }
    s_ep_in = alloc->ep_in[0];
    s_ep_out = alloc->ep_out[0];   /* 由 ep_out_same_num_as_in={0} 复用 IN[0] 的号码（=EP4） */
    s_bound = true;

    /* 把数据面端点交给 I0 服务；它的所有收发都只用这两个号（不硬编码） */
    webusb_proto_set_endpoints(s_ep_in, s_ep_out);

    ESP_LOGI(TAG, "IF2 控制已绑定：itf=%u ep_in=0x%02X ep_out=0x%02X str_idx=%u vendor_idx=%u",
             (unsigned)alloc->itf[0], (unsigned)s_ep_in, (unsigned)s_ep_out,
             (unsigned)alloc->itf_str_idx, (unsigned)alloc->vendor_idx);
    return ESP_OK;
}

/** EP4 OUT 有数据：**必须读干净**（在 TinyUSB 任务上下文，不可阻塞） */
static void ctrl_on_out(uint8_t ep_addr, void *user_ctx)
{
    (void)user_ctx;
    if (!s_bound || ep_addr != s_ep_out) {
        return;
    }
    g_uv_stats.ctrl_out_events++;
    /* 只把字节搬进 RX 环；解析在 webusb_proto 的任务里做（单线程，无锁） */
    webusb_proto_on_out(ep_addr);
}

static void ctrl_on_mount(bool mounted, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "主机 %s", mounted ? "已挂载（枚举完成）" : "已断开");
    webusb_proto_set_mounted(mounted);
}

/* =========================================================================
 * 访问器（usb_vendor.c 用）
 * ========================================================================= */

uint8_t uv_ctrl_ep_in(void)  { return s_ep_in; }
uint8_t uv_ctrl_ep_out(void) { return s_ep_out; }
bool    uv_ctrl_bound(void)  { return s_bound; }

/* =========================================================================
 * I1 功能对象
 * ========================================================================= */

const usb_function_t usb_vendor_fn_ctrl = {
    .name = "vendor-ctrl",
    .itf_count = 1,
    .itf_string = "MeidaP4 WebUSB",   /* 与 IF1 共用同一个字符串索引（§4） */
    .extra_string = NULL,
    .use_iad = false,                 /* 单接口功能不加 IAD（§6） */
    .ep_in_req = {
        [0] = { .xfer = USB_EP_XFER_BULK, .mps_hs = UV_BULK_MPS_HS, .mps_fs = UV_BULK_MPS_FS, .interval = 0 },
    },
    .ep_in_count = 1,
    .ep_out_req = {
        [0] = { .xfer = USB_EP_XFER_BULK, .mps_hs = UV_BULK_MPS_HS, .mps_fs = UV_BULK_MPS_FS, .interval = 0 },
    },
    .ep_out_count = 1,
    /* OUT[0] 复用 IN[0] 的号码 ⇒ 得到 EP4（frozen-allocation §2 / §2.1） */
    .ep_out_same_num_as_in = {
        [0] = 0,
        [1 ... USB_COMPOSITE_MAX_EP_PER_DIR - 1] = USB_EP_NO_PAIR,
    },
    .is_vendor = true,
    .build_desc = ctrl_build_desc,
    .init = ctrl_init,
    .on_out = ctrl_on_out,
    .on_mount = ctrl_on_mount,
    .on_control = NULL,               /* 不做私有控制请求；WebUSB/MS OS 2.0 由复合层处理 */
    .user_ctx = NULL,
    /* ⚠️ T2 起 IF1（视频）与 IF2（控制）**都**发 WCID —— Windows 只给 subset 里的接口装 winusb.sys */
    .wcid = true,
    .wcid_compatible_id = UV_WCID_COMPATIBLE_ID,   /* 复合层补成 WINUSB\0\0 */
    .wcid_device_interface_guid = UV_WCID_GUID,
};
