// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 探测阶段 USB 复合层
//   自定义描述符（device / FS+HS config / string / BOS / MS OS 2.0）
//   + WebUSB + WCID 供应商请求处理 + bulk 回环 + 心跳
//
// 契约：docs/probe-contract.md
//   §1 接口/端点分配   §2 描述符身份 + BOS + MS OS 2.0   §3.1 API   §4 线协议
//   **契约里的数字在探测期内不得单方面改动**，要改先找 Lead。
//
// 为什么完全自带描述符（不用 esp_tinyusb 的默认数组）：
//   esp_tinyusb 2.3.0 只在 CDC/MSC/MTP/NCM 类存在时才提供默认配置描述符
//   （descriptors_control.c:206/223），纯 VENDOR（模式 A）会直接返回 ESP_ERR_INVALID_ARG；
//   而且自带描述符才能把契约 §1 的端点表冻结下来。
//
// 线程模型（避免两个上下文同时写同一个 vendor TX FIFO）：
//   - tud_vendor_rx_cb() 跑在 TinyUSB 任务上下文 → 只把收到的字节搬进 stream buffer；
//   - 本组件创建的回环任务（probe_echo_task）是**唯一**的 TX 写入者，回环与心跳都由它发出；
//   - 心跳：空闲（无 OUT 数据）≥1 s 时主动写 "\nHB <uptime_ms>\n"（契约 §4）。
//
// 队列卫生（真机 1 MB 回环"整体位移 k 字节"的修复，A/B/C）：
//   真机现象：页面 PRNG 载荷"一致 4098 B / 失配 1044478 B" ⇒ 4098/1048576 ≈ 1/256 = **常数位移**
//   的指纹（吞吐 7.96 MB/s、RTT 0.497 ms 正常 ⇒ 用例健康跑完，不是丢数据/卡死）。
//   A. 挂载/卸载清理：event_cb 置 s_flush_requested，由回环任务（stream buffer 唯一消费者）
//      执行 xStreamBufferReset + tud_vendor_write_clear + 丢弃 pending。
//   B. 空闲→有数据的转变处丢弃排队心跳：判据是 s_hb_only_in_fifo（"自上次写 payload 以来
//      FIFO 里只写过心跳"），所以清它绝不会伤 payload。真机实测空闲期可积压 374 行 ≈ 4 KB 心跳，
//      正是"位移 k≈4098"的来源。
//   C. 只要"本地没有待写出的 payload"（pending=0 且 stream buffer 空）就能发心跳：既不会插进
//      payload 中间，又允许心跳在主机不读 IN 时正常积累（由 B 在数据到达时整段丢弃）。
//
// 生产者：usb-core

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tinyusb.h"  // esp_tinyusb：tinyusb_driver_install() / tinyusb_config_t
#include "tusb.h"     // TinyUSB：TUD_* 描述符宏 / tud_vendor_* / MS OS 2.0 枚举
#include "class/vendor/vendor_device.h"  // tud_vendor_* API 与 CFG_TUD_VENDOR_* 默认值

#include "usb_probe.h"

static const char *TAG = "usb_probe";

/* =====================================================================
 * 0. 编译期前提
 * ===================================================================== */

#if !CFG_TUD_VENDOR
#error "usb_probe 需要 CFG_TUD_VENDOR >= 1（CONFIG_TINYUSB_VENDOR_COUNT）"
#endif

#if !(SOC_USB_OTG_PERIPH_NUM > 1)
#error "本探测固件按 ESP32-P4 双口芯片编写（HS 口 = TINYUSB_PORT_HIGH_SPEED_0）"
#endif

/* 组件 Kconfig 的 MTP 开关必须与 tinyusb 类驱动一致，否则描述符表会和栈里的类驱动脱钩 */
#if CONFIG_USB_PROBE_MTP_ITF_ENABLE != CFG_TUD_MTP
#error "CONFIG_USB_PROBE_MTP_ITF_ENABLE 必须与 CONFIG_TINYUSB_MTP_ENABLED 一致"
#endif

/* S4 配置陷阱（docs/probe-verify.md §6）：
 * MTP 类驱动编进了固件、描述符里也有 MTP 接口，但 main.cpp 因为 CONFIG_PROBE_ENABLE_MTP=n
 * 不会调用 mtp_probe_start()（既没挂 storage 卷、也没 install/register MTP 后端）→
 * Windows 会枚举出一个"打不开的 MTP 设备"，比"没有 MTP"更难查。这里直接编译期拦住。 */
#if CFG_TUD_MTP && !CONFIG_PROBE_ENABLE_MTP
#error "CONFIG_TINYUSB_MTP_ENABLED=y 但 CONFIG_PROBE_ENABLE_MTP=n：MTP 接口会被枚举但后端没安装，请把两者设为一致"
#endif

/* =====================================================================
 * 1. 模式与冻结数字（契约 §1/§2）
 * ===================================================================== */

#if CFG_TUD_MTP
#define PROBE_MODE_B            1
#define PROBE_MODE_NAME         "B (MTP + vendor)"
#define PROBE_ITF_VENDOR        PROBE_ITF_VENDOR_B   /* IF1 */
#define PROBE_EP_VENDOR         PROBE_EP_VENDOR_B    /* EP3 */
#define PROBE_ITF_NUM_TOTAL     2
#define PROBE_ID_PRODUCT        PROBE_PID_MODE_B     /* 0x4002 */
#else
#define PROBE_MODE_B            0
#define PROBE_MODE_NAME         "A (vendor only)"
#define PROBE_ITF_VENDOR        PROBE_ITF_VENDOR_A   /* IF0 */
#define PROBE_EP_VENDOR         PROBE_EP_VENDOR_A    /* EP1 */
#define PROBE_ITF_NUM_TOTAL     1
#define PROBE_ID_PRODUCT        PROBE_PID_MODE_A     /* 0x4001 */
#endif

#define PROBE_STRID_LANGID      0
#define PROBE_STRID_MANUFACTURER 1
#define PROBE_STRID_PRODUCT     2
#define PROBE_STRID_SERIAL      3
#define PROBE_STRID_VENDOR_ITF  4
#define PROBE_STRID_MTP_ITF     5

/* ---- device 描述符的类字段（实验 A：模式 A 改成真正的非复合设备）----
 * CONFIG_USB_PROBE_DEVICE_CLASS_IAD=y → 0xEF/0x02/0x01（Misc/IAD，复合声明）
 * CONFIG_USB_PROBE_DEVICE_CLASS_IAD=n → 0xFF/0x00/0x00（Vendor Specific，非复合）
 * 真机证据：复合形态下 vendor 子接口（usbccgp child）**拿不到** GUID_DEVINTERFACE_USB_DEVICE
 * ({A5DCBF10-6530-11D2-901F-00C04FB951ED})——hub 驱动只把该 GUID 发给设备节点，从不发给
 * usbccgp 子接口（全机 DeviceClasses 里该 GUID 的 38 条记录中 &MI_ 命中 0），Chrome 因此
 * 枚举不到设备（chooser 空列表）。模式 A 用非复合声明可以让"父节点自己就是 WinUSB 设备"。 */
#if CONFIG_USB_PROBE_DEVICE_CLASS_IAD
#define PROBE_DEV_CLASS       TUSB_CLASS_MISC        /* 0xEF */
#define PROBE_DEV_SUBCLASS    MISC_SUBCLASS_COMMON   /* 0x02 */
#define PROBE_DEV_PROTOCOL    MISC_PROTOCOL_IAD      /* 0x01 */
#define PROBE_DEV_CLASS_DESC  "0xEF/0x02/0x01 Misc/IAD(复合)"
#else
#define PROBE_DEV_CLASS       0xFF
#define PROBE_DEV_SUBCLASS    0x00
#define PROBE_DEV_PROTOCOL    0x00
#define PROBE_DEV_CLASS_DESC  "0xFF/0x00/0x00 Vendor(非复合)"
#endif

#if PROBE_MODE_B && !CONFIG_USB_PROBE_DEVICE_CLASS_IAD
#error "模式 B 是 MTP+Vendor 复合设备：bDeviceClass 必须是 0xEF/0x02/0x01（Misc/IAD），不能改成非复合"
#endif

/* bcdDevice。契约 §2 原写 0x0100，真机实测必须改成 0x0101：
 *  - `USB\VID_303A&PID_4001&REV_0100` 这个硬件 ID 在本机**已经被另一台设备占用**
 *    （旧的 ESP32-S3/C3 USB 串口设备节点 `USB\VID_303A&PID_4001\123456`，service=usbccgp/usbser）；
 *    我们的 mode A 节点拿到同一个硬件 ID 后，Windows 直接套用了缓存的驱动状态
 *    （枚举后 `Service=WINUSB`、`DriverInfPath=winusb.inf`，但 setupapi 日志里**没有任何安装记录**），
 *    结果 winusb.sys 起不来：Device Manager Code 31 / ProblemStatus=0xC0000182 / 固件侧从未被
 *    SetConfiguration（stats mounted=0）。
 *  - 同时 bcdDevice 参与 Windows 的 MS OS 2.0 缓存键（`HKLM\SYSTEM\CurrentControlSet\Control\UsbFlags\
 *    <VID><PID><bcdDevice>`），改它可以让 Windows 重新读一遍 BOS/MS OS 描述符集——这正是
 *    mode B 里 MI_01 的 DeviceInterfaceGUIDs 一直没被写入的原因（缓存）。
 * 换 bcdDevice 不会影响网页按 PID 区分的模式判定。 */
#define PROBE_BCD_DEVICE        0x0102

/* 端点 packet size：USB 2.0 全速 bulk 上限 64，高速 bulk 上限 512（契约 §1 的 512 指 HS） */
#define PROBE_EP_BULK_MPS_HS      512
#define PROBE_EP_BULK_MPS_FS      64
#define PROBE_EP_MTP_EVT_MPS      64
#define PROBE_EP_MTP_EVT_INTERVAL 1

/* BOS 平台能力里的供应商请求码（契约 §2：WebUSB=0x01 / MS OS=0x02，两者必须不同）。
 * 这两个值**就是**各自 GET 请求的 bRequest，分发时必须与之严格对应：
 *   bRequest==0x01 -> WebUSB GET_URL      bRequest==0x02 -> MS OS 2.0 GET_MS_DESCRIPTOR
 * （Windows 实测发 C0 02 00 00 07 00 26 00 = bRequest 0x02 / wIndex 7） */
#define PROBE_BOS_VENDOR_CODE_WEBUSB 0x01
#define PROBE_BOS_VENDOR_CODE_MSOS20 0x02

/* WebUSB 着陆页索引。**必须是 0**：非 0 会诱使 Chrome 去取 GET_URL，
 * 而探测期没有着陆页（GET_URL 一律 stall）→ Chrome 可能在 chooser 里把设备丢掉
 * （真机现象：file:// 下 requestDevice 报"找不到兼容设备"，但设备确实在主机上）。
 * 改回非 0 之前必须同时实现 GET_URL（见 tud_vendor_control_xfer_cb 里的说明）。 */
#define PROBE_WEBUSB_LANDING_PAGE    0

#define PROBE_MSOS20_WINDOWS_VERSION 0x06030000u
/* GET_MS_DESCRIPTOR 的 wIndex：MS 文档/Zephyr 用 7，本项目契约为 0 → 两者都接受 */
#define PROBE_MSOS20_DESCRIPTOR_INDEX 0x07

_Static_assert(PROBE_BOS_VENDOR_CODE_WEBUSB != PROBE_BOS_VENDOR_CODE_MSOS20,
               "WebUSB 与 MS OS 2.0 必须用不同的 bVendorCode（否则主机请求会打架）");
_Static_assert(PROBE_WEBUSB_LANDING_PAGE <= 0xFF, "iLandingPage 是单字节字段");

#define PROBE_BOS_TOTAL_LEN \
    (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN) /* 5+24+28 = 57 */

#define PROBE_MSOS20_SET_HEADER_LEN    0x000A
#define PROBE_MSOS20_SUBSET_HEADER_LEN 0x0008
#define PROBE_MSOS20_COMPAT_ID_LEN     0x0014

/* ---- MS OS 2.0 Registry Property：把 device interface GUID 写进 Device Parameters ----
 * 目的：让 vendor 接口在 Windows 上真正注册出 device interface，Chrome 才能打开它。
 * 真机证据：MI_01（service=WINUSB）**一个 device interface 都没注册**，且 `MI_01\Device Parameters`
 *   下没有该值；同机另一个同为 usbccgp 子接口、同为 WINUSB 的设备有该值 → 注册被"该值是否存在"门控。
 *   winusb.inf 的 [WINUSB.NT.HW] 是空占位段（winusb.inf:52-53），全文件唯一带该值的段是 ADB 专用段
 *   （winusb.inf:96）→ WCID 自动绑定 winusb.sys 并不会写这个值，必须由本描述符提供。
 * 布局（MS OS 2.0 规范）：
 *   wLength(2) wDescriptorType(2)=0x0004 wPropertyDataType(2) wPropertyNameLength(2)
 *   PropertyName(UTF-16LE) wPropertyDataLength(2) PropertyData
 *   → wLength = 10 + name + data
 *   （对照 tinyusb examples/device/webusb_serial/src/usb_descriptors.c:214-223：132 = 10 + 42 + 80 ✓）
 *
 * ⚠️ 真机关键结论（2026-10-16，本机 Win11 26100）：**值的名字/类型必须用下面 STYLE=1 的写法**。
 *   STYLE=0（复数 DeviceInterfaceGUIDs + REG_MULTI_SZ，tinyusb 例子/网上最常见的写法）：
 *     Windows 会把值正确写进 Device Parameters（我用 PowerShell 都读到了两个 GUID），
 *     但 **winusb.sys 随后起不来**：Device Manager Code 31、ProblemStatus=0xC0000182、
 *     设备再也没被 SetConfiguration（mode A 固件 stats mounted=0）；mode B 的 MI_01 同样从
 *     "problem=0/无接口" 变成 "Code 31"。两种 GUID 列表（含/不含 A5DCBF10）都复现。
 *   STYLE=1（单数 DeviceInterfaceGUID + REG_SZ + 单个 GUID，78 B）：
 *     正是 Microsoft "WinUSB device" 文档给的写法（bPropertyName=DeviceInterfaceGUID、
 *     wPropertyNameLength=40、bPropertyData 78 B），也是本机唯一能正常启动的 WINUSB 设备
 *     （ASUS VID_0B05&PID_19AF&MI_00，它的值就叫 DeviceInterfaceGUID、类型 REG_SZ）的写法。 */
#define PROBE_GUID_USB_DEVICE            "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"
#define PROBE_GUID_WINUSB                "{DEE824EF-729B-4A0E-9C14-B7117D33A817}"

#define PROBE_MSOS20_REGPROP_STYLE       1   /* 0 = 复数/REG_MULTI_SZ；1 = 单数/REG_SZ（真机唯一可用） */

#if PROBE_MSOS20_REGPROP_STYLE == 0
#define PROBE_MSOS20_REGPROP_NAME_ASCII  "DeviceInterfaceGUIDs"
#define PROBE_MSOS20_REGPROP_NAME_LEN    42   /* (20 字符 + NUL) × 2 */
#define PROBE_MSOS20_REGPROP_DATA_TYPE   0x0007 /* REG_MULTI_SZ */
/* 是否把 {A5DCBF10-…}(GUID_DEVINTERFACE_USB_DEVICE) 也放进列表。MS 文档指出该接口类由系统
 * USB hub 驱动注册（guid-devinterface-usb-device），功能驱动去注册它不是设计用途。
 * 真机实测两种列表都让 winusb.sys 起不来，所以这个开关对 STYLE=0 不是关键变量。 */
#define PROBE_MSOS20_REGPROP_USE_GUID_A  1
#if PROBE_MSOS20_REGPROP_USE_GUID_A
#define PROBE_MSOS20_REGPROP_DATA_LEN    158  /* ((38+1) + (38+1) + 1) × 2 */
#else
#define PROBE_MSOS20_REGPROP_DATA_LEN    80   /* (38+1+1) × 2 */
#endif
#else
#define PROBE_MSOS20_REGPROP_NAME_ASCII  "DeviceInterfaceGUID"
#define PROBE_MSOS20_REGPROP_NAME_LEN    40   /* (19 字符 + NUL) × 2 */
#define PROBE_MSOS20_REGPROP_DATA_TYPE   0x0001 /* REG_SZ */
#define PROBE_MSOS20_REGPROP_USE_GUID_A  1    /* 单数形式只发一个 GUID，固定发 A5DCBF10 */
#define PROBE_MSOS20_REGPROP_DATA_LEN    78   /* (38 + 1) × 2：GUID 文本 + 单个终止 NUL */
#endif
#define PROBE_MSOS20_REGPROP_LEN \
    (2 + 2 + 2 + 2 + PROBE_MSOS20_REGPROP_NAME_LEN + 2 + PROBE_MSOS20_REGPROP_DATA_LEN)

_Static_assert(sizeof(PROBE_GUID_USB_DEVICE) - 1 == 38, "GUID A 长度必须是 38 字符");
_Static_assert(sizeof(PROBE_GUID_WINUSB) - 1 == 38, "GUID B 长度必须是 38 字符");
_Static_assert(PROBE_MSOS20_REGPROP_NAME_LEN == (sizeof(PROBE_MSOS20_REGPROP_NAME_ASCII)) * 2,
               "property name 长度 = (ASCII 长度 + NUL) × 2");
_Static_assert(PROBE_MSOS20_REGPROP_LEN == 10 + PROBE_MSOS20_REGPROP_NAME_LEN + PROBE_MSOS20_REGPROP_DATA_LEN,
               "Registry Property wLength 核算不一致");
#if PROBE_MSOS20_REGPROP_STYLE == 0
#if PROBE_MSOS20_REGPROP_USE_GUID_A
_Static_assert(PROBE_MSOS20_REGPROP_DATA_LEN == ((38 + 1) + (38 + 1) + 1) * 2, "PropertyData 长度核算不一致");
_Static_assert(PROBE_MSOS20_REGPROP_LEN == 210, "Registry Property wLength 应为 0xD2");
#else
_Static_assert(PROBE_MSOS20_REGPROP_DATA_LEN == (38 + 1 + 1) * 2, "PropertyData 长度核算不一致");
_Static_assert(PROBE_MSOS20_REGPROP_LEN == 132, "Registry Property wLength 应为 0x84");
#endif
#else
_Static_assert(PROBE_MSOS20_REGPROP_DATA_LEN == (38 + 1) * 2, "PropertyData 长度核算不一致");
_Static_assert(PROBE_MSOS20_REGPROP_LEN == 128, "Registry Property wLength 应为 0x80");
#endif
_Static_assert(PROBE_MSOS20_REGPROP_LEN == 10 + PROBE_MSOS20_REGPROP_NAME_LEN + PROBE_MSOS20_REGPROP_DATA_LEN,
               "Registry Property wLength 核算不一致");

#define PROBE_MSOS20_SUBSET_LEN \
    (PROBE_MSOS20_SUBSET_HEADER_LEN + PROBE_MSOS20_COMPAT_ID_LEN + PROBE_MSOS20_REGPROP_LEN)

/* 描述符集的结构必须与"设备是不是复合设备"匹配（真机实测）：
 *  - 模式 B（bDeviceClass=0xEF/0x02/0x01 复合）：CompatibleID/RegistryProperty 放在 **Function
 *    Subset Header** 里，指向 vendor 接口 → Windows 把 winusb.sys 绑到 MI_01（已实测生效）。
 *  - 模式 A（0xFF/0x00/0x00 非复合）：必须是 **设备级** 特性（Set Header 后直接跟 CompatibleID），
 *    不能有 Function Subset Header。真机实测：带 Function Subset Header 时 Windows 读完了整个
 *    248 B 描述符集（串口日志有 vendor ctrl #1 wLength=248），但没有应用兼容 ID →
 *    `USB\MS_COMP_WINUSB` 不在 CompatibleIds 里 → ProblemCode=28（驱动未安装）、设备压根没被
 *    SetConfiguration（stats mounted=0）。改成设备级后按 MS 文档/tinyusb dfu 示例的形态。
 * 两种形态的 Registry Property 都保留（DeviceInterfaceGUIDs）。 */
#if PROBE_MODE_B
#define PROBE_MSOS20_DESC_SET_LEN \
    (PROBE_MSOS20_SET_HEADER_LEN + PROBE_MSOS20_SUBSET_HEADER_LEN + PROBE_MSOS20_COMPAT_ID_LEN + PROBE_MSOS20_REGPROP_LEN)
#else
#define PROBE_MSOS20_DESC_SET_LEN \
    (PROBE_MSOS20_SET_HEADER_LEN + PROBE_MSOS20_COMPAT_ID_LEN + PROBE_MSOS20_REGPROP_LEN)
#endif

/* 期望的集合总长（按 结构 × GUID 开关 组合核算；改了任何一段都会在这里报编译错） */
#if PROBE_MODE_B
#define PROBE_MSOS20_DESC_SET_EXPECTED \
    (PROBE_MSOS20_SET_HEADER_LEN + PROBE_MSOS20_SUBSET_HEADER_LEN + PROBE_MSOS20_COMPAT_ID_LEN + PROBE_MSOS20_REGPROP_LEN)
#else
#define PROBE_MSOS20_DESC_SET_EXPECTED \
    (PROBE_MSOS20_SET_HEADER_LEN + PROBE_MSOS20_COMPAT_ID_LEN + PROBE_MSOS20_REGPROP_LEN)
#endif
_Static_assert(PROBE_MSOS20_DESC_SET_LEN == PROBE_MSOS20_DESC_SET_EXPECTED,
               "MS OS 2.0 集长度常量与组成部分不一致");

#define PROBE_VENDOR_DESC_LEN   TUD_VENDOR_DESC_LEN /* 9+7+7 = 23 */
#define PROBE_MTP_DESC_LEN      TUD_MTP_DESC_LEN    /* 9+7+7+7 = 30 */
#if PROBE_MODE_B
#define PROBE_CFG_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + PROBE_MTP_DESC_LEN + PROBE_VENDOR_DESC_LEN) /* 62 */
#else
#define PROBE_CFG_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + PROBE_VENDOR_DESC_LEN)                      /* 32 */
#endif

/* 契约 §1：模式 b 下 MTP(EP1/EP2) 与 Vendor(EP3) 不得撞号 */
#if PROBE_MODE_B
_Static_assert(PROBE_EP_MTP_EVT_IN != PROBE_EP_MTP_DATA, "EP 号重复：MTP 事件/数据");
_Static_assert(PROBE_EP_MTP_EVT_IN != PROBE_EP_VENDOR, "EP 号重复：MTP 事件/Vendor");
_Static_assert(PROBE_EP_MTP_DATA != PROBE_EP_VENDOR, "EP 号重复：MTP 数据/Vendor");
#endif

/* 回环/心跳参数 */
#define PROBE_ECHO_RING_SIZE    8192
#define PROBE_ECHO_TASK_STACK   4096
#define PROBE_ECHO_TASK_PRIO    5
#define PROBE_ECHO_WAIT_MS      20
#define PROBE_HEARTBEAT_IDLE_US (1000 * 1000)
#define PROBE_USB_TASK_STACK    4096
#define PROBE_USB_TASK_PRIO     5
#define PROBE_USB_TASK_CORE     1

/* 供应商控制请求的前 N 条用 INFO 级原样打印（真机对照用；避免主机轮询时刷屏） */
#define PROBE_CTRL_LOG_LIMIT    16

/* =====================================================================
 * 2. 描述符（契约 §2）
 * ===================================================================== */

static const tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = PROBE_BCD_USB,                 /* 0x0210：<0x0210 主机根本不取 BOS */
    .bDeviceClass = PROBE_DEV_CLASS,         /* 复合 0xEF/0x02/0x01 或非复合 0xFF/0x00/0x00 */
    .bDeviceSubClass = PROBE_DEV_SUBCLASS,
    .bDeviceProtocol = PROBE_DEV_PROTOCOL,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PROBE_VID,                   /* 0x303A */
    .idProduct = PROBE_ID_PRODUCT,           /* A=0x4001 / B=0x4002 */
    .bcdDevice = PROBE_BCD_DEVICE,
    .iManufacturer = PROBE_STRID_MANUFACTURER,
    .iProduct = PROBE_STRID_PRODUCT,
    .iSerialNumber = PROBE_STRID_SERIAL,
    .bNumConfigurations = 1,
};

/* HS 口需要 qualifier（esp_tinyusb 在 HS 口上必须拿到它，否则用默认的 bcdUSB=0x0200）。
 * qualifier 的类字段必须与 device 描述符逐字节一致（USB 2.0 §9.6.2）。 */
static const tusb_desc_device_qualifier_t s_desc_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = PROBE_BCD_USB,
    .bDeviceClass = PROBE_DEV_CLASS,
    .bDeviceSubClass = PROBE_DEV_SUBCLASS,
    .bDeviceProtocol = PROBE_DEV_PROTOCOL,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

/* 配置描述符 bmAttributes：总线供电（bit7 由 TUD_CONFIG_DESCRIPTOR 自行置位），
 * 不声明远端唤醒——探测固件没有实现 tud_remote_wakeup()。 */
#define PROBE_CFG_ATTRIBUTES  0x00

/* ---- 全速配置描述符（模式 A：1 个 Vendor 接口；模式 B：MTP IF0 + Vendor IF1）---- */
static const uint8_t s_desc_cfg_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, PROBE_ITF_NUM_TOTAL, 0, PROBE_CFG_TOTAL_LEN,
                          PROBE_CFG_ATTRIBUTES, 100),
#if PROBE_MODE_B
    /* 接口号, 字符串索引, 事件 EP(IN,interrupt), 事件包长, 轮询间隔, 数据 OUT/IN 号, 包长 */
    TUD_MTP_DESCRIPTOR(PROBE_ITF_MTP, PROBE_STRID_MTP_ITF,
                       0x80 | PROBE_EP_MTP_EVT_IN, PROBE_EP_MTP_EVT_MPS, PROBE_EP_MTP_EVT_INTERVAL,
                       PROBE_EP_MTP_DATA, 0x80 | PROBE_EP_MTP_DATA, PROBE_EP_BULK_MPS_FS),
#endif
    /* 接口号, 字符串索引, EP OUT 号, EP IN 号, 包长 */
    TUD_VENDOR_DESCRIPTOR(PROBE_ITF_VENDOR, PROBE_STRID_VENDOR_ITF,
                          PROBE_EP_VENDOR, 0x80 | PROBE_EP_VENDOR, PROBE_EP_BULK_MPS_FS),
};

/* ---- 高速配置描述符（结构同上，bulk 包长 512）---- */
static const uint8_t s_desc_cfg_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, PROBE_ITF_NUM_TOTAL, 0, PROBE_CFG_TOTAL_LEN,
                          PROBE_CFG_ATTRIBUTES, 100),
#if PROBE_MODE_B
    TUD_MTP_DESCRIPTOR(PROBE_ITF_MTP, PROBE_STRID_MTP_ITF,
                       0x80 | PROBE_EP_MTP_EVT_IN, PROBE_EP_MTP_EVT_MPS, PROBE_EP_MTP_EVT_INTERVAL,
                       PROBE_EP_MTP_DATA, 0x80 | PROBE_EP_MTP_DATA, PROBE_EP_BULK_MPS_HS),
#endif
    TUD_VENDOR_DESCRIPTOR(PROBE_ITF_VENDOR, PROBE_STRID_VENDOR_ITF,
                          PROBE_EP_VENDOR, 0x80 | PROBE_EP_VENDOR, PROBE_EP_BULK_MPS_HS),
};

/* ---- 字符串描述符（≤8 条，索引见契约 §2）----
 * str[0] 不是字符串而是 LANGID 的两字节小端值（esp_tinyusb descriptors_control.c:141）。*/
static const char s_langid_en_us[] = { 0x09, 0x04 }; /* 0x0409 */
static const char *s_string_desc[] = {
    s_langid_en_us,        /* 0 LANGID */
    "MeidaP4",             /* 1 Manufacturer */
    "MeidaP4 Probe",       /* 2 Product */
    "PROBE-D3",            /* 3 Serial（契约 §2；改这个值 = 迫使 Windows 建新实例） */
    "MeidaP4 WebUSB",      /* 4 Vendor 接口 */
#if PROBE_MODE_B
    "MeidaP4 Storage",     /* 5 MTP 接口 */
#endif
};
#define PROBE_STRID_COUNT (sizeof(s_string_desc) / sizeof(s_string_desc[0]))

_Static_assert(PROBE_STRID_COUNT <= 8, "esp_tinyusb 的字符串描述符上限是 8 条");

/* ---- BOS：两个平台能力（WebUSB + MS OS 2.0）---- */
static const uint8_t s_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(PROBE_BOS_TOTAL_LEN, 2),
    /* UUID {3408b638-09a9-47a0-8bfd-a0768815b665}, bcdVersion=0x0100, bVendorCode=0x01,
     * iLandingPage=0（不带着陆页：Chrome 因此不会去取 GET_URL，见 PROBE_WEBUSB_LANDING_PAGE 注释） */
    TUD_BOS_WEBUSB_DESCRIPTOR(PROBE_BOS_VENDOR_CODE_WEBUSB, PROBE_WEBUSB_LANDING_PAGE),
    /* UUID {d8dd60df-4589-4cc7-9cd2-659d9e648a9f}, dwWindowsVersion=0x06030000, bVendorCode=0x02 */
    TUD_BOS_MS_OS_20_DESCRIPTOR(PROBE_MSOS20_DESC_SET_LEN, PROBE_BOS_VENDOR_CODE_MSOS20),
};

/* ---- MS OS 2.0 描述符集：Set Header + Function Subset Header + CompatibleID + Registry Property ----
 * 只给 vendor 接口发 WINUSB 兼容 ID；MTP 那一路交给 Windows 自带的 wpdmtp.sys（契约 §2）。
 * Registry Property 是"实验 B"：把 DeviceInterfaceGUIDs 写进 vendor 接口的 Device Parameters，
 * 让 winusb.sys 真正注册出 device interface（详见上面的长度宏注释）。 */
#define PROBE_U16(c) (uint8_t)(c), 0x00   /* ASCII 字符 -> UTF-16LE 两字节 */

static const uint8_t s_msos20_desc_set[] = {
    /* Set Header：wLength, wDescriptorType, dwWindowsVersion, wTotalLength */
    U16_TO_U8S_LE(PROBE_MSOS20_SET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(PROBE_MSOS20_WINDOWS_VERSION),
    U16_TO_U8S_LE(PROBE_MSOS20_DESC_SET_LEN),

    /* Function Subset Header：bFirstInterfaceNumber=vendor 接口, bReserved, wSubsetLength
     * **仅复合设备（模式 B）使用**；模式 A 是非复合设备，兼容 ID 必须挂在设备级。 */
#if PROBE_MODE_B
    U16_TO_U8S_LE(PROBE_MSOS20_SUBSET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    PROBE_ITF_VENDOR, 0x00,
    U16_TO_U8S_LE(PROBE_MSOS20_SUBSET_LEN),
#endif

    /* Compatible ID Feature：CompatibleID="WINUSB\0\0", SubCompatibleID=8×0x00 */
    U16_TO_U8S_LE(PROBE_MSOS20_COMPAT_ID_LEN), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* Registry Property Feature 头：wLength, wDescriptorType,
     * wPropertyDataType（STYLE=1 → REG_SZ 0x0001 / STYLE=0 → REG_MULTI_SZ 0x0007）,
     * wPropertyNameLength */
    U16_TO_U8S_LE(PROBE_MSOS20_REGPROP_LEN), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(PROBE_MSOS20_REGPROP_DATA_TYPE), U16_TO_U8S_LE(PROBE_MSOS20_REGPROP_NAME_LEN),

    /* PropertyName（UTF-16LE）：STYLE=1 = "DeviceInterfaceGUID"(19+NUL=40 B)，
     * STYLE=0 = "DeviceInterfaceGUIDs"(20+NUL=42 B) */
    PROBE_U16('D'), PROBE_U16('e'), PROBE_U16('v'), PROBE_U16('i'), PROBE_U16('c'), PROBE_U16('e'),
    PROBE_U16('I'), PROBE_U16('n'), PROBE_U16('t'), PROBE_U16('e'), PROBE_U16('r'), PROBE_U16('f'),
    PROBE_U16('a'), PROBE_U16('c'), PROBE_U16('e'), PROBE_U16('G'), PROBE_U16('U'), PROBE_U16('I'),
    PROBE_U16('D'),
#if PROBE_MSOS20_REGPROP_STYLE == 0
    PROBE_U16('s'),
#endif
    0x00, 0x00,                       /* 字符串终止 NUL */

    /* wPropertyDataLength */
    U16_TO_U8S_LE(PROBE_MSOS20_REGPROP_DATA_LEN),

    /* PropertyData = GUID 的 UTF-16LE 文本
     *   "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"   <- Chrome/Edge 认的 device interface GUID
     *   STYLE=0 时后面再跟 "{DEE824EF-…}"（GUID_DEVINTERFACE_WINUSB） */
    /* GUID A = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}" (38 chars) */
#if PROBE_MSOS20_REGPROP_USE_GUID_A
    PROBE_U16('{'), PROBE_U16('A'), PROBE_U16('5'), PROBE_U16('D'), PROBE_U16('C'),
    PROBE_U16('B'), PROBE_U16('F'), PROBE_U16('1'), PROBE_U16('0'), PROBE_U16('-'),
    PROBE_U16('6'), PROBE_U16('5'), PROBE_U16('3'), PROBE_U16('0'), PROBE_U16('-'),
    PROBE_U16('1'), PROBE_U16('1'), PROBE_U16('D'), PROBE_U16('2'), PROBE_U16('-'),
    PROBE_U16('9'), PROBE_U16('0'), PROBE_U16('1'), PROBE_U16('F'), PROBE_U16('-'),
    PROBE_U16('0'), PROBE_U16('0'), PROBE_U16('C'), PROBE_U16('0'), PROBE_U16('4'),
    PROBE_U16('F'), PROBE_U16('B'), PROBE_U16('9'), PROBE_U16('5'), PROBE_U16('1'),
    PROBE_U16('E'), PROBE_U16('D'), PROBE_U16('}'),
    0x00, 0x00,                       /* GUID A 的字符串终止 NUL */
#endif
#if PROBE_MSOS20_REGPROP_STYLE == 0
    /* GUID B = "{DEE824EF-729B-4A0E-9C14-B7117D33A817}" (38 chars) */
    PROBE_U16('{'), PROBE_U16('D'), PROBE_U16('E'), PROBE_U16('E'), PROBE_U16('8'),
    PROBE_U16('2'), PROBE_U16('4'), PROBE_U16('E'), PROBE_U16('F'), PROBE_U16('-'),
    PROBE_U16('7'), PROBE_U16('2'), PROBE_U16('9'), PROBE_U16('B'), PROBE_U16('-'),
    PROBE_U16('4'), PROBE_U16('A'), PROBE_U16('0'), PROBE_U16('E'), PROBE_U16('-'),
    PROBE_U16('9'), PROBE_U16('C'), PROBE_U16('1'), PROBE_U16('4'), PROBE_U16('-'),
    PROBE_U16('B'), PROBE_U16('7'), PROBE_U16('1'), PROBE_U16('1'), PROBE_U16('7'),
    PROBE_U16('D'), PROBE_U16('3'), PROBE_U16('3'), PROBE_U16('A'), PROBE_U16('8'),
    PROBE_U16('1'), PROBE_U16('7'), PROBE_U16('}'),
    0x00, 0x00, 0x00, 0x00,           /* GUID B 终止 NUL + REG_MULTI_SZ 结尾 NUL（STYLE=0） */
#endif
};

/* 编译期就把数组长度钉死：任何一段/开关改动导致的长度不符都会在这里报错 */
_Static_assert(sizeof(s_msos20_desc_set) == PROBE_MSOS20_DESC_SET_LEN,
               "s_msos20_desc_set 实际长度与 PROBE_MSOS20_DESC_SET_LEN 不一致");

/* =====================================================================
 * 3. 运行期状态
 * ===================================================================== */

static volatile uint32_t s_out_bytes;
static volatile uint32_t s_out_packets;
static volatile uint32_t s_in_bytes;
static volatile uint32_t s_in_packets;
static volatile uint32_t s_echo_errors;
static volatile uint32_t s_ctrl_requests;
static volatile uint32_t s_ctrl_stalls;
static volatile uint32_t s_ctrl_log_seq;
static volatile bool s_mounted;

static StreamBufferHandle_t s_echo_sb;
static bool s_started;

/* ---- 队列卫生（真机 1 MB 回环"整体位移 k 字节"的修复）----
 * 现象：页面载荷是 PRNG，实测"一致 4098 B / 失配 1044478 B" ⇒ 4098/1048576 ≈ 1/256
 *       = **常数位移**的指纹（不是丢数据、不是卡死：吞吐 7.96 MB/s、RTT 0.497 ms 都正常）。
 * 机制：设备侧任何"排在新回环数据之前"的字节都会造成全局位移——空闲期排队的心跳、
 *       上一轮用例没排空的残留。而全工程原先**没有任何队列清理**。
 * 对策：
 *   A. event_cb 在挂载/卸载时请求清理；由回环任务（stream buffer 的唯一消费者）执行
 *      SB reset + TX FIFO clear + 丢弃本地 pending（跨任务 reset stream buffer 会因
 *      "有任务在等"而 pdFAIL，所以必须由拥有者自己清）。
 *   B. 空闲→有数据的转变处丢弃排队心跳：只有在"回环路径已完全排空"（s_echo_drained）
 *      时才允许清 TX FIFO，此时 FIFO 里**只可能**有心跳字节，丢它不会伤 payload。
 *   C. 有回环积压（pending / stream buffer 非空）时不发心跳，避免心跳插进 payload 中间。
 */
static volatile bool s_flush_requested;   /* 由 event_cb 置位，回环任务执行 */
/* "TX FIFO 里自上次写入 payload 以来只写过心跳" —— 这是 B 能安全清 FIFO 的**唯一**依据。
 * 不能用"FIFO 是否为空"判断（payload 尾巴没发完时 FIFO 也非空）；也不能用"是否排空"，
 * 因为主机不读 IN 时心跳会在 FIFO 里积累（真机实测旧版积压 374 行 ≈ 4 KB，正是位移 k 的来源）。
 * 维护规则：
 *   置 true  —— 只在"写这条心跳之前 FIFO 恰好是空的（drained）"时（此后 FIFO 里只可能是心跳）；
 *   置 false —— 任何一次写入 payload 之后、以及 B 清空之后。
 * 于是 B 清 FIFO 时不可能丢掉 payload。 */
static bool s_hb_only_in_fifo = true;
/* B 的可观测性：丢弃积压心跳时打一行限流日志（前 8 次 + 之后每 5 s 最多 1 条）。
 * 为什么需要它：修前/修后用主机工具往往都是 0 失配（工具自己会 drain 对齐），测不出差别；
 * 有了这行日志，"B 真的把空闲期积压的心跳清掉了"就是设备侧可直接核对的证据。 */
static uint32_t s_flush_log_count;
static int64_t s_flush_log_last_us;

/* 只允许 TinyUSB 任务上下文访问（rx_cb 专用），因此可以是 static 而非栈上 */
static uint8_t s_rx_scratch[512];

/* =====================================================================
 * 4. 描述符自检（契约 §1/§2：数字对不上一律拒绝启动）
 * ===================================================================== */

#define PROBE_CHECK(cond, ...)                            \
    do {                                                  \
        if (!(cond)) {                                    \
            ESP_LOGE(TAG, "desc self-check: " __VA_ARGS__); \
            return ESP_ERR_INVALID_STATE;                 \
        }                                                 \
    } while (0)

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 把 UTF-16LE 区域与 ASCII 串逐字符比较（每个字符占 2 字节，高字节必须是 0） */
static bool utf16le_equals(const uint8_t *p, const char *ascii, size_t chars)
{
    for (size_t i = 0; i < chars; i++) {
        if (p[2 * i] != (uint8_t)ascii[i] || p[2 * i + 1] != 0x00) {
            return false;
        }
    }
    return true;
}

typedef struct {
    uint8_t addr;
    uint8_t xfer;
    uint16_t mps;
} probe_ep_info_t;

typedef struct {
    uint16_t total_len;
    uint8_t itf_count;
    uint8_t itf_num[2];
    uint8_t itf_ep_count[2];
    uint8_t itf_class[2];
    uint8_t ep_count;
    probe_ep_info_t ep[5];
} probe_cfg_info_t;

static esp_err_t probe_parse_cfg(const uint8_t *cfg, probe_cfg_info_t *out)
{
    memset(out, 0, sizeof(*out));

    const uint8_t *p = cfg;
    PROBE_CHECK(p[0] == TUD_CONFIG_DESC_LEN, "config bLength=%u", p[0]);
    PROBE_CHECK(p[1] == TUSB_DESC_CONFIGURATION, "config bDescriptorType=0x%02X", p[1]);

    out->total_len = rd_le16(p + 2);
    out->itf_count = p[4];
    PROBE_CHECK(out->total_len <= 255, "wTotalLength=%u 过大", out->total_len);

    const uint8_t *end = cfg + out->total_len;
    p += TUD_CONFIG_DESC_LEN;

    int itf = -1;
    while (p < end) {
        PROBE_CHECK((size_t)(end - p) >= 2, "描述符被截断");
        const uint8_t len = p[0];
        const uint8_t type = p[1];
        PROBE_CHECK(len >= 2 && (p + len) <= end, "非法 bLength=%u", len);

        if (type == TUSB_DESC_INTERFACE) {
            PROBE_CHECK(itf + 1 < 2, "接口数超过 2");
            itf++;
            out->itf_num[itf] = p[2];
            out->itf_ep_count[itf] = p[4];
            out->itf_class[itf] = p[5];
        } else if (type == TUSB_DESC_ENDPOINT) {
            PROBE_CHECK(itf >= 0 && out->ep_count < 5, "端点位置/数量异常");
            out->ep[out->ep_count].addr = p[2];
            out->ep[out->ep_count].xfer = (uint8_t)(p[3] & 0x03);
            out->ep[out->ep_count].mps = rd_le16(p + 4);
            out->ep_count++;
        }
        p += len;
    }
    PROBE_CHECK(p == end, "描述符链没有精确落在 wTotalLength=%u 上", out->total_len);
    PROBE_CHECK(out->itf_count == (uint8_t)(itf + 1), "bNumInterfaces=%u 与实际接口数 %d 不一致",
                out->itf_count, itf + 1);
    return ESP_OK;
}

static bool probe_ep_is(const probe_cfg_info_t *info, uint8_t idx, uint8_t addr, uint8_t xfer, uint16_t mps)
{
    if (idx >= info->ep_count) {
        return false;
    }
    return info->ep[idx].addr == addr && info->ep[idx].xfer == xfer && info->ep[idx].mps == mps;
}

static esp_err_t probe_check_cfg(const probe_cfg_info_t *info, bool hs)
{
    const uint16_t bulk = hs ? PROBE_EP_BULK_MPS_HS : PROBE_EP_BULK_MPS_FS;
    const char *speed = hs ? "HS" : "FS";

    PROBE_CHECK(info->total_len == PROBE_CFG_TOTAL_LEN,
                "%s wTotalLength=%u，期望 %u", speed, info->total_len, PROBE_CFG_TOTAL_LEN);
    PROBE_CHECK(info->itf_count == PROBE_ITF_NUM_TOTAL,
                "%s bNumInterfaces=%u，期望 %u", speed, info->itf_count, PROBE_ITF_NUM_TOTAL);

#if PROBE_MODE_B
    /* IF0 = MTP：3 个端点（EP1 interrupt IN 64 + EP2 bulk IN/OUT 512） */
    PROBE_CHECK(info->itf_num[0] == PROBE_ITF_MTP && info->itf_ep_count[0] == 3,
                "%s MTP 接口号=%u EP 数=%u", speed, info->itf_num[0], info->itf_ep_count[0]);
    PROBE_CHECK(info->itf_class[0] == TUSB_CLASS_IMAGE, "%s MTP 接口类=0x%02X", speed, info->itf_class[0]);
    PROBE_CHECK(probe_ep_is(info, 0, 0x80 | PROBE_EP_MTP_EVT_IN, TUSB_XFER_INTERRUPT, PROBE_EP_MTP_EVT_MPS),
                "%s MTP 事件端点异常", speed);
    PROBE_CHECK(probe_ep_is(info, 1, PROBE_EP_MTP_DATA, TUSB_XFER_BULK, bulk), "%s MTP OUT 端点异常", speed);
    PROBE_CHECK(probe_ep_is(info, 2, 0x80 | PROBE_EP_MTP_DATA, TUSB_XFER_BULK, bulk), "%s MTP IN 端点异常", speed);
    /* IF1 = Vendor：EP3 bulk IN/OUT */
    PROBE_CHECK(info->itf_num[1] == PROBE_ITF_VENDOR && info->itf_ep_count[1] == 2,
                "%s Vendor 接口号=%u EP 数=%u", speed, info->itf_num[1], info->itf_ep_count[1]);
    PROBE_CHECK(info->itf_class[1] == TUSB_CLASS_VENDOR_SPECIFIC, "%s Vendor 接口类=0x%02X", speed, info->itf_class[1]);
    PROBE_CHECK(probe_ep_is(info, 3, PROBE_EP_VENDOR, TUSB_XFER_BULK, bulk), "%s Vendor OUT 端点异常", speed);
    PROBE_CHECK(probe_ep_is(info, 4, 0x80 | PROBE_EP_VENDOR, TUSB_XFER_BULK, bulk), "%s Vendor IN 端点异常", speed);
    PROBE_CHECK(info->ep_count == 5, "%s 端点数=%u，期望 5", speed, info->ep_count);
#else
    /* IF0 = Vendor：EP1 bulk IN/OUT */
    PROBE_CHECK(info->itf_num[0] == PROBE_ITF_VENDOR && info->itf_ep_count[0] == 2,
                "%s Vendor 接口号=%u EP 数=%u", speed, info->itf_num[0], info->itf_ep_count[0]);
    PROBE_CHECK(info->itf_class[0] == TUSB_CLASS_VENDOR_SPECIFIC, "%s Vendor 接口类=0x%02X", speed, info->itf_class[0]);
    PROBE_CHECK(probe_ep_is(info, 0, PROBE_EP_VENDOR, TUSB_XFER_BULK, bulk), "%s Vendor OUT 端点异常", speed);
    PROBE_CHECK(probe_ep_is(info, 1, 0x80 | PROBE_EP_VENDOR, TUSB_XFER_BULK, bulk), "%s Vendor IN 端点异常", speed);
    PROBE_CHECK(info->ep_count == 2, "%s 端点数=%u，期望 2", speed, info->ep_count);
#endif

    /* 契约 §1 / dwc2_esp32.h:80：P4 HS 口 IN 端点（含 EP0）上限 8 → 用户 IN ≤ 7 */
    uint8_t ep_in = 0;
    for (uint8_t i = 0; i < info->ep_count; i++) {
        if (info->ep[i].addr & 0x80) {
            ep_in++;
        }
    }
    PROBE_CHECK(ep_in == (PROBE_MODE_B ? 3 : 1), "%s IN 端点数=%u 与契约不一致", speed, ep_in);
    PROBE_CHECK(ep_in <= 7, "%s IN 端点数=%u 超出 P4 HS 口上限（含 EP0 共 8）", speed, ep_in);
    return ESP_OK;
}

static esp_err_t probe_check_bos(void)
{
    static const uint8_t webusb_uuid[16] = { TUD_BOS_WEBUSB_UUID };
    static const uint8_t msos20_uuid[16] = { TUD_BOS_MS_OS_20_UUID };

    PROBE_CHECK(sizeof(s_desc_bos) == PROBE_BOS_TOTAL_LEN, "BOS 长度=%u，期望 %u",
                (unsigned)sizeof(s_desc_bos), PROBE_BOS_TOTAL_LEN);
    PROBE_CHECK(s_desc_bos[0] == TUD_BOS_DESC_LEN && s_desc_bos[1] == TUSB_DESC_BOS, "BOS 头异常");
    PROBE_CHECK(rd_le16(&s_desc_bos[2]) == PROBE_BOS_TOTAL_LEN, "BOS wTotalLength 异常");
    PROBE_CHECK(s_desc_bos[4] == 2, "BOS bNumDeviceCaps=%u，期望 2", s_desc_bos[4]);

    /* WebUSB 平台能力 @5，24 字节 */
    PROBE_CHECK(s_desc_bos[5] == TUD_BOS_WEBUSB_DESC_LEN, "WebUSB 能力长度=%u", s_desc_bos[5]);
    PROBE_CHECK(s_desc_bos[6] == TUSB_DESC_DEVICE_CAPABILITY &&
                s_desc_bos[7] == DEVICE_CAPABILITY_PLATFORM, "WebUSB 能力类型异常");
    PROBE_CHECK(memcmp(&s_desc_bos[9], webusb_uuid, sizeof(webusb_uuid)) == 0, "WebUSB UUID 不匹配");
    PROBE_CHECK(rd_le16(&s_desc_bos[25]) == 0x0100, "WebUSB bcdVersion 异常");
    PROBE_CHECK(s_desc_bos[27] == PROBE_BOS_VENDOR_CODE_WEBUSB, "WebUSB bVendorCode=0x%02X", s_desc_bos[27]);
    PROBE_CHECK(s_desc_bos[28] == PROBE_WEBUSB_LANDING_PAGE, "WebUSB iLandingPage=%u", s_desc_bos[28]);

    /* MS OS 2.0 平台能力 @29，28 字节 */
    PROBE_CHECK(s_desc_bos[29] == TUD_BOS_MICROSOFT_OS_DESC_LEN, "MS OS 能力长度=%u", s_desc_bos[29]);
    PROBE_CHECK(s_desc_bos[30] == TUSB_DESC_DEVICE_CAPABILITY &&
                s_desc_bos[31] == DEVICE_CAPABILITY_PLATFORM, "MS OS 能力类型异常");
    PROBE_CHECK(memcmp(&s_desc_bos[33], msos20_uuid, sizeof(msos20_uuid)) == 0, "MS OS UUID 不匹配");
    PROBE_CHECK(rd_le32(&s_desc_bos[49]) == PROBE_MSOS20_WINDOWS_VERSION, "MS OS dwWindowsVersion 异常");
    PROBE_CHECK(rd_le16(&s_desc_bos[53]) == sizeof(s_msos20_desc_set), "MS OS 描述符集总长字段异常");
    PROBE_CHECK(s_desc_bos[55] == PROBE_BOS_VENDOR_CODE_MSOS20, "MS OS bVendorCode=0x%02X", s_desc_bos[55]);
    return ESP_OK;
}

static esp_err_t probe_check_msos20(void)
{
    const uint8_t *s = s_msos20_desc_set;

    PROBE_CHECK(sizeof(s_msos20_desc_set) == PROBE_MSOS20_DESC_SET_LEN, "MS OS 集长度=%u，期望 %u",
                (unsigned)sizeof(s_msos20_desc_set), PROBE_MSOS20_DESC_SET_LEN);

    /* Set Header */
    PROBE_CHECK(rd_le16(&s[0]) == PROBE_MSOS20_SET_HEADER_LEN &&
                rd_le16(&s[2]) == MS_OS_20_SET_HEADER_DESCRIPTOR, "MS OS Set Header 异常");
    PROBE_CHECK(rd_le32(&s[4]) == PROBE_MSOS20_WINDOWS_VERSION, "MS OS 版本字段异常");
    PROBE_CHECK(rd_le16(&s[8]) == PROBE_MSOS20_DESC_SET_LEN, "MS OS wTotalLength 异常");

    /* 逐段遍历：按 wLength 前进，同时验证每一段的类型/内容，最后必须精确落在 wTotalLength 上
     * （这就是 verifier S1 要求的"wTotalLength 与逐段实测一致"）。 */
    uint16_t off = PROBE_MSOS20_SET_HEADER_LEN;

#if PROBE_MODE_B
    /* Function Subset Header（只有复合设备才有） */
    PROBE_CHECK(rd_le16(&s[off]) == PROBE_MSOS20_SUBSET_HEADER_LEN &&
                rd_le16(&s[off + 2]) == MS_OS_20_SUBSET_HEADER_FUNCTION, "MS OS Subset Header 异常");
    PROBE_CHECK(s[off + 4] == PROBE_ITF_VENDOR, "MS OS bFirstInterfaceNumber=%u，期望 %u",
                s[off + 4], PROBE_ITF_VENDOR);
    PROBE_CHECK(rd_le16(&s[off + 6]) == PROBE_MSOS20_SUBSET_LEN, "MS OS wSubsetLength 异常");
    off = (uint16_t)(off + PROBE_MSOS20_SUBSET_HEADER_LEN);
#endif

    /* Compatible ID Feature */
    PROBE_CHECK(rd_le16(&s[off]) == PROBE_MSOS20_COMPAT_ID_LEN &&
                rd_le16(&s[off + 2]) == MS_OS_20_FEATURE_COMPATBLE_ID, "MS OS CompatibleID 头异常");
    PROBE_CHECK(memcmp(&s[off + 4], "WINUSB\0\0", 8) == 0, "MS OS CompatibleID 不是 WINUSB");
    for (int i = off + 12; i < off + 20; i++) {
        PROBE_CHECK(s[i] == 0x00, "MS OS SubCompatibleID 必须全 0");
    }
    off = (uint16_t)(off + PROBE_MSOS20_COMPAT_ID_LEN);

    /* Registry Property Feature（实验 B） */
    const uint8_t *rp = &s[off];
    const uint16_t name_len = PROBE_MSOS20_REGPROP_NAME_LEN;
    const uint16_t name_chars = name_len / 2 - 1;   /* 去掉终止 NUL */
    PROBE_CHECK(rd_le16(&rp[0]) == PROBE_MSOS20_REGPROP_LEN, "RegProp wLength=%u，期望 %u",
                rd_le16(&rp[0]), PROBE_MSOS20_REGPROP_LEN);
    PROBE_CHECK(rd_le16(&rp[2]) == MS_OS_20_FEATURE_REG_PROPERTY, "RegProp wDescriptorType 异常");
    PROBE_CHECK(rd_le16(&rp[4]) == PROBE_MSOS20_REGPROP_DATA_TYPE, "RegProp wPropertyDataType=%u，期望 %u",
                rd_le16(&rp[4]), PROBE_MSOS20_REGPROP_DATA_TYPE);
    PROBE_CHECK(rd_le16(&rp[6]) == name_len, "RegProp wPropertyNameLength 异常");
    PROBE_CHECK(utf16le_equals(&rp[8], PROBE_MSOS20_REGPROP_NAME_ASCII, name_chars),
                "PropertyName 不是 %s", PROBE_MSOS20_REGPROP_NAME_ASCII);
    PROBE_CHECK(rp[8 + name_len - 2] == 0x00 && rp[8 + name_len - 1] == 0x00, "PropertyName 缺少终止 NUL");
    PROBE_CHECK(rd_le16(&rp[8 + name_len]) == PROBE_MSOS20_REGPROP_DATA_LEN,
                "RegProp wPropertyDataLength=%u，期望 %u", rd_le16(&rp[8 + name_len]),
                PROBE_MSOS20_REGPROP_DATA_LEN);
    const uint8_t *pd = &rp[10 + name_len];
#if PROBE_MSOS20_REGPROP_USE_GUID_A
    PROBE_CHECK(utf16le_equals(pd, PROBE_GUID_USB_DEVICE, 38), "PropertyData GUID A 不匹配");
    PROBE_CHECK(pd[76] == 0x00 && pd[77] == 0x00, "PropertyData GUID A 缺少终止 NUL");
#endif
#if PROBE_MSOS20_REGPROP_STYLE == 1
    /* REG_SZ：GUID A 后面只需要一个终止 NUL（数据总长 78 = 76 + 2） */
    PROBE_CHECK(PROBE_MSOS20_REGPROP_DATA_LEN == 78, "REG_SZ 形式的 PropertyData 长度应为 78");
#else
    PROBE_CHECK(utf16le_equals(&pd[78], PROBE_GUID_WINUSB, 38), "PropertyData GUID B 不匹配");
    PROBE_CHECK(pd[154] == 0x00 && pd[155] == 0x00 && pd[156] == 0x00 && pd[157] == 0x00,
                "PropertyData 结尾必须是双 NUL（REG_MULTI_SZ）");
#endif
    off = (uint16_t)(off + PROBE_MSOS20_REGPROP_LEN);

    PROBE_CHECK(off == rd_le16(&s[8]), "MS OS 分段长度之和 %u != wTotalLength %u", off, rd_le16(&s[8]));
    return ESP_OK;
}

static esp_err_t probe_check_device_desc(void)
{
    PROBE_CHECK(s_desc_device.bLength == sizeof(tusb_desc_device_t) &&
                s_desc_device.bDescriptorType == TUSB_DESC_DEVICE, "device 描述符头异常");
    PROBE_CHECK(s_desc_device.bcdUSB >= 0x0210, "device bcdUSB=0x%04X 必须 >= 0x0210（否则主机不取 BOS）",
                s_desc_device.bcdUSB);
    PROBE_CHECK(s_desc_device.bDeviceClass == PROBE_DEV_CLASS &&
                s_desc_device.bDeviceSubClass == PROBE_DEV_SUBCLASS &&
                s_desc_device.bDeviceProtocol == PROBE_DEV_PROTOCOL,
                "device 类字段=0x%02X/0x%02X/0x%02X 与预期 %s 不一致",
                s_desc_device.bDeviceClass, s_desc_device.bDeviceSubClass, s_desc_device.bDeviceProtocol,
                PROBE_DEV_CLASS_DESC);
    PROBE_CHECK(s_desc_qualifier.bLength == sizeof(tusb_desc_device_qualifier_t) &&
                s_desc_qualifier.bDescriptorType == TUSB_DESC_DEVICE_QUALIFIER, "qualifier 描述符头异常");
    PROBE_CHECK(s_desc_qualifier.bDeviceClass == s_desc_device.bDeviceClass &&
                s_desc_qualifier.bDeviceSubClass == s_desc_device.bDeviceSubClass &&
                s_desc_qualifier.bDeviceProtocol == s_desc_device.bDeviceProtocol,
                "qualifier 的类字段必须与 device 描述符一致（USB 2.0 §9.6.2）");
    PROBE_CHECK(s_desc_device.idVendor == PROBE_VID && s_desc_device.idProduct == PROBE_ID_PRODUCT,
                "VID=0x%04X PID=0x%04X 与契约不符", s_desc_device.idVendor, s_desc_device.idProduct);
    return ESP_OK;
}

static esp_err_t probe_desc_selfcheck(void)
{
    probe_cfg_info_t fs = { 0 };
    probe_cfg_info_t hs = { 0 };
    esp_err_t err;

    err = probe_check_device_desc();
    if (err != ESP_OK) {
        return err;
    }
    err = probe_parse_cfg(s_desc_cfg_fs, &fs);
    if (err != ESP_OK) {
        return err;
    }
    err = probe_parse_cfg(s_desc_cfg_hs, &hs);
    if (err != ESP_OK) {
        return err;
    }
    err = probe_check_cfg(&fs, false);
    if (err != ESP_OK) {
        return err;
    }
    err = probe_check_cfg(&hs, true);
    if (err != ESP_OK) {
        return err;
    }
    err = probe_check_bos();
    if (err != ESP_OK) {
        return err;
    }
    err = probe_check_msos20();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "desc self-check OK | %s | dev %s | IF=%u | FS cfg=%u B | HS cfg=%u B | BOS=%u B | MSOS20=%u B | vendor IF%u EP 0x%02X/0x%02X",
             PROBE_MODE_NAME, PROBE_DEV_CLASS_DESC, PROBE_ITF_NUM_TOTAL, PROBE_CFG_TOTAL_LEN, PROBE_CFG_TOTAL_LEN,
             PROBE_BOS_TOTAL_LEN, PROBE_MSOS20_DESC_SET_LEN, PROBE_ITF_VENDOR,
             PROBE_EP_VENDOR, 0x80 | PROBE_EP_VENDOR);
    return ESP_OK;
}

/* =====================================================================
 * 5. TinyUSB 回调
 * ===================================================================== */

/* 注意：esp_tinyusb 2.3.0 的 tinyusb.c 已经提供**强符号** tud_mount_cb()/tud_umount_cb()
 * （esp_tinyusb 内部要转发 ATTACHED/DETACHED 事件），所以这里不能再定义它们，
 * 否则链接期 multiple definition。改用 tinyusb_config_t.event_cb 达到同样效果。 */
static void probe_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_mounted = true;
        /* A（会话开始）：清 RX FIFO（本任务上下文，安全）+ 请求回环任务清 TX 侧 */
#if CFG_TUD_VENDOR_TXRX_BUFFERED
        tud_vendor_read_flush();
#endif
        s_flush_requested = true;
        ESP_LOGI(TAG, "MOUNT  (host 已枚举完成 / SetConfiguration 非 0)");
        break;
    case TINYUSB_EVENT_DETACHED:
        s_mounted = false;
        /* A（会话结束）：清 TX 侧残留（RX 侧由 tinyusb 的 vendord_reset 在总线复位时清，
         * 这里不再 read_flush —— 免得在 teardown 期间重新挂一个 RX transfer）。 */
        s_flush_requested = true;
        ESP_LOGI(TAG, "UMOUNT (SetConfiguration(0) 或拔线)");
        break;
    default:
        break;
    }
}

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    /* DATA/ACK 阶段：栈已经拿到数据，必须返回 true，返回 false 会把控制传输 stall 掉 */
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    s_ctrl_requests++;

    /* 真机对照用：把收到的供应商请求原样打出来（INFO 级，只打前 PROBE_CTRL_LOG_LIMIT 条，避免刷屏）。
     * 关键是把 bRequest（= BOS 里声明的 bVendorCode）与 wIndex 打出来：
     *   WebUSB GET_URL       : bRequest=0x01
     *   MS OS GET_MS_DESCRIPTOR: bRequest=0x02, wIndex=7（Windows 实测） */
    if (s_ctrl_log_seq < PROBE_CTRL_LOG_LIMIT) {
        s_ctrl_log_seq++;
        ESP_LOGI(TAG, "vendor ctrl #%u: bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u",
                 s_ctrl_log_seq, request->bmRequestType, request->bRequest, request->wValue,
                 request->wIndex, request->wLength);
    }

    /* ---- MS OS 2.0 GET_MS_DESCRIPTOR：bmRequestType=0xC0, bRequest=0x02(vendor code) ---- */
    if (request->bRequest == PROBE_BOS_VENDOR_CODE_MSOS20) {
        /* 主机先以 wLength=0x0A 取 Set Header，再以 wLength=全集取整set → 按 wLength 分段应答。
         * 契约 §2 原写 wIndex=0；MS 文档/Zephyr/tinyusb 示例用 7，真机 Windows 实测也是 7，两者都接受。 */
        if (request->wIndex == 0x0000 || request->wIndex == PROBE_MSOS20_DESCRIPTOR_INDEX) {
            const uint16_t xfer_len = tu_min16((uint16_t)sizeof(s_msos20_desc_set), request->wLength);
            if (xfer_len == 0) {
                ESP_LOGW(TAG, "MS OS GET_MS_DESCRIPTOR wLength=0 -> stall");
                s_ctrl_stalls++;
                return false;
            }
            ESP_LOGI(TAG, "MS OS GET_MS_DESCRIPTOR wIndex=%u wLength=%u -> 回 %u B",
                     request->wIndex, request->wLength, xfer_len);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_msos20_desc_set, xfer_len);
        }
        ESP_LOGW(TAG, "MS OS GET_MS_DESCRIPTOR wIndex=%u 不认识 -> stall", request->wIndex);
        s_ctrl_stalls++;
        return false;
    }

    /* ---- WebUSB GET_URL：bmRequestType=0xC0, bRequest=0x01(WebUSB 的 bVendorCode) ----
     * BOS 里 iLandingPage=0，Chrome 不会发这个请求；真发了就 stall（探测期没有着陆页）。
     * 若将来把 PROBE_WEBUSB_LANDING_PAGE 改成非 0，这里必须改成返回 URL 描述符：
     *   { bLength = 3+len, bDescriptorType = 0x03, bScheme(0=http/1=https/0xFF=none), UTF-8 URL } */
    if (request->bRequest == PROBE_BOS_VENDOR_CODE_WEBUSB) {
        ESP_LOGW(TAG, "WebUSB GET_URL (wIndex=%u wLength=%u) 未实现 -> stall（BOS 里 iLandingPage=0，不应发生）",
                 request->wIndex, request->wLength);
        s_ctrl_stalls++;
        return false;
    }

    ESP_LOGW(TAG, "未知供应商请求 bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u -> stall",
             request->bmRequestType, request->bRequest, request->wValue, request->wIndex, request->wLength);
    s_ctrl_stalls++;
    return false;
}

/* 回环：只搬数据，不发 TX（TX 由 probe_echo_task 单点写入，避免与 TX 完成回调并发改 FIFO）
 * 签名跟随 tinyusb 的兼容开关（本工程 CFG_TUD_API_V0_19_COMPAT 默认为 1 → bufsize 是 uint16_t）。 */
#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
#else
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize)
#endif
{
    (void)idx;

    if (s_echo_sb == NULL) {
        return;
    }

    s_out_packets++;

#if CFG_TUD_VENDOR_TXRX_BUFFERED
    /* FIFO 模式：回调里 buffer/bufsize 无意义（vendor_device.h:191），数据在 RX FIFO 里 */
    (void)buffer;
    (void)bufsize;
    uint32_t avail = tud_vendor_available();
    while (avail > 0) {
        const uint32_t want = avail < sizeof(s_rx_scratch) ? avail : (uint32_t)sizeof(s_rx_scratch);
        const uint32_t got = tud_vendor_read(s_rx_scratch, want);
        if (got == 0) {
            break;
        }
        s_out_bytes += got;

        const size_t queued = xStreamBufferSend(s_echo_sb, s_rx_scratch, got, 0);
        if (queued < got) {
            /* 应用侧缓冲满：这部分数据回不去 → 主机的逐字节比对会发现 */
            s_echo_errors++;
            if ((s_echo_errors % 64) == 1) {
                ESP_LOGW(TAG, "echo 缓冲满，丢弃 %u B（累计 echo_errors=%" PRIu32 "）",
                         (unsigned)(got - queued), s_echo_errors);
            }
        }
        avail = tud_vendor_available();
    }
#else
    /* 直传模式：回调直接带数据 */
    if (buffer != NULL && bufsize > 0) {
        s_out_bytes += bufsize;
        const size_t queued = xStreamBufferSend(s_echo_sb, buffer, bufsize, 0);
        if (queued < bufsize) {
            s_echo_errors++;
        }
    }
#endif
}

/* =====================================================================
 * 6. 回环 / 心跳任务（唯一的 TX 写入者）
 * ===================================================================== */

static void probe_echo_task(void *arg)
{
    (void)arg;

    static uint8_t buf[1024];
    size_t pending = 0;
    int64_t last_rx_us = esp_timer_get_time();
    uint32_t hb_seq = 0;

    ESP_LOGI(TAG, "echo/heartbeat task started");

    while (true) {
        /* A：event_cb 请求的会话级清理（由本任务执行，因为本任务是 stream buffer 的唯一消费者；
         *    从别的任务 reset 会因为"有接收者在等"而 pdFAIL） */
        if (s_flush_requested) {
            s_flush_requested = false;
            pending = 0;
            (void)xStreamBufferReset(s_echo_sb);
#if CFG_TUD_VENDOR_TXRX_BUFFERED
            tud_vendor_write_clear();
#endif
            s_hb_only_in_fifo = true;   /* FIFO 刚被清空 → 之后只可能是心跳 */
            last_rx_us = esp_timer_get_time();
            ESP_LOGD(TAG, "queues flushed (mount/umount)");
        }

        if (!tud_mounted()) {
            /* 未枚举（或已拔线）：清掉残余，避免下一轮误发旧数据 */
            pending = 0;
            vTaskDelay(pdMS_TO_TICKS(PROBE_ECHO_WAIT_MS));
            last_rx_us = esp_timer_get_time();
            continue;
        }

        if (pending == 0) {
            const size_t got = xStreamBufferReceive(s_echo_sb, buf, sizeof(buf),
                                                    pdMS_TO_TICKS(PROBE_ECHO_WAIT_MS));
            pending = got;
            if (got > 0) {
                last_rx_us = esp_timer_get_time();
                /* B（空闲 → 有数据的转变）：此时 TX FIFO 里若"自上次 payload 以来只写过心跳"
                 * （s_hb_only_in_fifo），就把它整段丢掉，保证本用例的 IN 流从回环数据的第一个
                 * 字节开始（这正是"整体位移 k 字节"的根因：真机测得空闲期积压 374 行 ≈ 4 KB 心跳）。
                 * 判据不能用"FIFO 是否为空"——payload 尾巴没发完时也非空，那时绝不能清。 */
#if CFG_TUD_VENDOR_TXRX_BUFFERED
                if (s_hb_only_in_fifo) {
                    /* 清之前先量一下积压了多少字节（= 空闲期排队、尚未发出去的心跳）
                     * 用于验收：'idle->data flush TX: dropped N B …' */
                    const uint32_t queued =
                        (uint32_t)CFG_TUD_VENDOR_TX_BUFSIZE - tud_vendor_write_available();
                    tud_vendor_write_clear();
                    s_hb_only_in_fifo = false;
                    if (queued > 0) {
                        const int64_t t_us = esp_timer_get_time();
                        if (s_flush_log_count < 8 ||
                            (t_us - s_flush_log_last_us) >= 5 * 1000 * 1000) {
                            s_flush_log_count++;
                            s_flush_log_last_us = t_us;
                            ESP_LOGI(TAG,
                                     "idle->data flush TX: dropped %u B queued heartbeat before echo",
                                     (unsigned)queued);
                        }
                    }
                }
#endif
            }
        }

        /* 1) 把回环数据推回主机（TX FIFO 满就留到下一轮，形成背压） */
        if (pending > 0) {
            size_t sent = 0;
            while (sent < pending) {
                const uint32_t avail = tud_vendor_write_available();
                if (avail == 0) {
                    break;
                }
                size_t n = pending - sent;
                if (n > (size_t)avail) {
                    n = (size_t)avail;
                }
                const uint32_t w = tud_vendor_write(buf + sent, n);
                if (w == 0) {
                    break;
                }
                sent += w;
            }
            if (sent > 0) {
#if CFG_TUD_VENDOR_TXRX_BUFFERED
                tud_vendor_write_flush();
#endif
                s_in_bytes += sent;
                s_in_packets++;
                s_hb_only_in_fifo = false;   /* FIFO 里已经有 payload → B 不许再清 */
            }
            if (sent < pending) {
                /* 还有没发完的：挪到缓冲区头部，让出 CPU 等 TX FIFO 排空后继续 */
                memmove(buf, buf + sent, pending - sent);
                pending -= sent;
                vTaskDelay(1);
            } else {
                pending = 0;
            }
        }

        /* 本地是否没有待写出的 payload（C 的判据之一）+ TX FIFO 是否恰好为空（B 的置位条件）。 */
        const bool no_local_backlog = (pending == 0 && xStreamBufferBytesAvailable(s_echo_sb) == 0);
        bool fifo_empty = false;
        if (no_local_backlog) {
#if CFG_TUD_VENDOR_TXRX_BUFFERED
            fifo_empty = (tud_vendor_write_available() >= (uint32_t)CFG_TUD_VENDOR_TX_BUFSIZE);
#else
            fifo_empty = (tud_vendor_write_available() > 0);
#endif
        }

        /* 2) 空闲 ≥1 s 主动发心跳（契约 §4："\nHB <uptime_ms>\n"）。
         * C：只要"本地没有待写出的 payload"就能发 —— 这样心跳不会插进 payload 中间
         *    （payload 一定在 FIFO 里排在心跳之前，或还没收到）。
         *    注意**不能**再要求"FIFO 为空"：主机不读 IN 时心跳会在 FIFO 里正常积累
         *    （契约语义：空闲就该有心跳可读），B 会在第一块回环数据到达时整段丢掉它们。 */
        const int64_t now = esp_timer_get_time();
        if (tud_mounted() && no_local_backlog && (now - last_rx_us) >= PROBE_HEARTBEAT_IDLE_US) {
            char hb[48];
            const int len = snprintf(hb, sizeof(hb), "\nHB %" PRIu64 "\n", (uint64_t)(now / 1000));
            if (len > 0 && len < (int)sizeof(hb)) {
                /* 只有"写这条心跳之前 FIFO 恰好为空"才能保证之后 FIFO 里只含心跳 */
                if (fifo_empty) {
                    s_hb_only_in_fifo = true;
                }
                const uint32_t w = tud_vendor_write(hb, (uint32_t)len);
#if CFG_TUD_VENDOR_TXRX_BUFFERED
                tud_vendor_write_flush();
#endif
                last_rx_us = now; /* 心跳本身不算"活动"，但重置计时避免连续刷 */
                if (w == (uint32_t)len) {
                    hb_seq++;
                    ESP_LOGD(TAG, "heartbeat #%" PRIu32 " sent (uptime %" PRIu64 " ms)", hb_seq, (uint64_t)(now / 1000));
                }
            }
        }
    }
}

/* =====================================================================
 * 7. 公开 API（契约 §3.1）
 * ===================================================================== */

esp_err_t usb_probe_start(void)
{
    if (s_started) {
        ESP_LOGE(TAG, "usb_probe_start() 只能调用一次（重复调用）");
        return ESP_ERR_INVALID_STATE;
    }

    /* 描述符自检：数字对不上就不装驱动 */
    esp_err_t err = probe_desc_selfcheck();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "描述符自检失败，拒绝启动（契约 §1/§2 的数字必须一致）");
        return err;
    }

    s_echo_sb = xStreamBufferCreate(PROBE_ECHO_RING_SIZE, 1);
    if (s_echo_sb == NULL) {
        ESP_LOGE(TAG, "创建 echo stream buffer 失败");
        return ESP_ERR_NO_MEM;
    }

    const tinyusb_config_t cfg = {
        /* P4 是双口芯片：不显式指定 HS 口会装到 FS 口上（契约 §3.1） */
        .port = TINYUSB_PORT_HIGH_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,   /* 探测板由主机供电，不做 VBUS 监测 */
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = PROBE_USB_TASK_STACK,
            .priority = PROBE_USB_TASK_PRIO,
            .xCoreID = PROBE_USB_TASK_CORE,
        },
        .descriptor = {
            .device = &s_desc_device,
            .qualifier = &s_desc_qualifier,
            .string = s_string_desc,
            .string_count = (int)PROBE_STRID_COUNT,
            .full_speed_config = s_desc_cfg_fs,
            .high_speed_config = s_desc_cfg_hs,
        },
        .pm_lock_enable = false,
        .event_cb = probe_event_cb,
        .event_arg = NULL,
    };

    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install() 失败: %s", esp_err_to_name(err));
        vStreamBufferDelete(s_echo_sb);
        s_echo_sb = NULL;
        return err;
    }

    if (xTaskCreate(probe_echo_task, "usb_probe_echo", PROBE_ECHO_TASK_STACK, NULL,
                    PROBE_ECHO_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建回环任务失败（驱动已装，回环/心跳不可用）");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "TinyUSB 已装 | mode %s | VID 0x%04X PID 0x%04X | FS cfg %u B | HS cfg %u B | BOS %u B | vendor IF%u EP 0x%02X/0x%02X",
             PROBE_MODE_NAME, PROBE_VID, PROBE_ID_PRODUCT, (unsigned)sizeof(s_desc_cfg_fs),
             (unsigned)sizeof(s_desc_cfg_hs), (unsigned)sizeof(s_desc_bos), PROBE_ITF_VENDOR,
             PROBE_EP_VENDOR, 0x80 | PROBE_EP_VENDOR);
    return ESP_OK;
}

void usb_probe_get_stats(usb_probe_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->out_bytes = s_out_bytes;
    out->in_bytes = s_in_bytes;
    out->out_packets = s_out_packets;
    out->in_packets = s_in_packets;
    out->echo_errors = s_echo_errors;
    out->ctrl_requests = s_ctrl_requests;
    out->ctrl_stalls = s_ctrl_stalls;
    out->mounted = s_mounted;
}

uint32_t usb_probe_config_desc_len(bool high_speed)
{
    return high_speed ? (uint32_t)sizeof(s_desc_cfg_hs) : (uint32_t)sizeof(s_desc_cfg_fs);
}
