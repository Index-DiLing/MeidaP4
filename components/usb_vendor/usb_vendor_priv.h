// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · usb_vendor 内部共享声明（私有，不在 include/ 里）
//
// 只放两件东西：
//   1. USB 描述符的**最小原语**（接口 / 端点），供两个功能的 build_desc 回调复用；
//   2. 两个 usb_function_t 的声明（定义在 usb_vendor_video.c / usb_vendor_ctrl.c）。
//
// ⚠️ 这里出现的数字只有 USB 规范常量与 frozen-allocation §2 的**包大小**；
//    接口号 / 端点号 / 字符串索引一律来自 usb_fn_alloc_t。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usb_composite.h"

#include "usb_vendor.h"

/* ---- 冻结的包大小（docs/contracts/frozen-allocation.md §2：bulk HS 512 / FS 64） ---- */
#define UV_BULK_MPS_HS   512
#define UV_BULK_MPS_FS   64

/* ---- WCID（MS OS 2.0）共用身份 ----
 * 2026-10-22 裁决（T2）：**IF1 视频 + IF2 控制都发 WCID**。
 * 真机实测：Windows 只给 MS OS Function Subset 里列出的接口装 winusb.sys；
 * 只给 IF2 发 WCID 时 MI_01 无驱动 ⇒ Chrome/主机工具打不开 EP3 ⇒ 视频永远收不到。
 * 两个接口用同一个 DeviceInterfaceGUID（真机验证过的 {A5DCBF10-…}，Chrome 只认它）。 */
#define UV_WCID_GUID  "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"
#define UV_WCID_COMPATIBLE_ID  "WINUSB"

/* ---- USB 描述符类型/属性（USB 2.0 规范常量，不是本项目的分配数字） ---- */
#define USB_DT_INTERFACE     0x04
#define USB_DT_ENDPOINT      0x05
#define USB_CLASS_VENDOR     0xFF
#define USB_EP_ATTR_BULK     0x02
#define USB_EP_DESC_LEN      7
#define USB_ITF_DESC_LEN     9

/** 接口描述符（9 B）：class=FF/00/00（Vendor 专用），iInterface 由复合层分配 */
static inline size_t uv_itf_desc(uint8_t *b, uint8_t itf_num, uint8_t n_ep, uint8_t itf_str_idx)
{
    b[0] = USB_ITF_DESC_LEN;
    b[1] = USB_DT_INTERFACE;
    b[2] = itf_num;
    b[3] = 0;                 /* bAlternateSetting */
    b[4] = n_ep;
    b[5] = USB_CLASS_VENDOR;
    b[6] = 0x00;
    b[7] = 0x00;
    b[8] = itf_str_idx;
    return USB_ITF_DESC_LEN;
}

/** 端点描述符（7 B）：只支持 bulk（本方案全程不用等时，plan §1） */
static inline size_t uv_ep_desc(uint8_t *b, uint8_t ep_addr, uint16_t mps, uint8_t interval)
{
    b[0] = USB_EP_DESC_LEN;
    b[1] = USB_DT_ENDPOINT;
    b[2] = ep_addr;           /* IN 含 0x80 位；OUT 不含 */
    b[3] = USB_EP_ATTR_BULK;
    b[4] = (uint8_t)(mps & 0xFF);
    b[5] = (uint8_t)((mps >> 8) & 0xFF);
    b[6] = interval;          /* bulk 恒 0 */
    return USB_EP_DESC_LEN;
}

/* ---- 两个功能对象 ---- */
extern const usb_function_t usb_vendor_fn_video;   /* IF1 · 视频（只有 IN） */
extern const usb_function_t usb_vendor_fn_ctrl;    /* IF2 · 控制（IN + OUT，带 WCID） */

/* ---- 共享诊断计数（定义在 usb_vendor.c）
        视频任务写 video_*、TinyUSB 任务写 ctrl_out_* —— 只用于统计，32 bit 自增足够 ---- */
extern usb_vendor_stats_t g_uv_stats;

/* ---- 视频发送任务（usb_vendor_video.c）---- */
esp_err_t uv_video_task_start(void);

/* ---- 控制功能给 webusb_proto 绑定端点用的访问器（usb_vendor_ctrl.c）---- */
uint8_t uv_ctrl_ep_in(void);
uint8_t uv_ctrl_ep_out(void);
bool    uv_ctrl_bound(void);
