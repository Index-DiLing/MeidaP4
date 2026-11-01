// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I1 契约 —— USB 复合层「功能注册」接口（**冻结**）
//
//   生产者（本文件 + 实现）：Lead
//   消费者：components/mtp_backend（WS-storage）· components/usb_vendor（WS-webusb）
//           · components/usb_net（WS-net）
//
// ⚠️ 冻结规则（plan §2/I1）：**任何工作流都不得直接构造或修改配置描述符**。
//    各功能只「注册描述符片段 + 三个回调」，接口号 / 端点号 / 字符串索引 / FIFO 预算
//    全部由本组件统一分配。数字来源见 docs/contracts/frozen-allocation.md。
//
// 设计要点：
//   * 描述符在**运行时**组装（不是宏拼装），这样各功能的片段可独立演进；
//   * 组装是**纯计算**：`usb_composite_prepare()` 不碰 TinyUSB，可单独离线自检；
//   * 复合层自己拥有 device / qualifier / string / fs_cfg / hs_cfg / BOS / MS OS 2.0，
//     通过 esp_tinyusb 的 `tinyusb_desc_config_t` 整包交给 `tinyusb_driver_install()`。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 冻结的资源上限（表见 docs/contracts/frozen-allocation.md §1/§2）
 * ========================================================================= */

/** 最多注册几个功能（本方案 4：MTP / Vendor-Video / Vendor-Ctrl / NCM） */
#define USB_COMPOSITE_MAX_FUNCTIONS     6
/** 最多接口数（本方案 5：IF0 MTP / IF1 视频 / IF2 控制 / IF3 NCM-C / IF4 NCM-D） */
#define USB_COMPOSITE_MAX_ITF           6
/** 每方向最多端点（HS 口用户 IN ≤ 7，见 frozen-allocation §2） */
#define USB_COMPOSITE_MAX_EP_PER_DIR    7
/** 单个配置描述符缓冲上限（FS 与 HS 各一份，运行时分配） */
#define USB_COMPOSITE_CFG_DESC_MAX      512
/** MS OS 2.0 描述符集缓冲上限 */
#define USB_COMPOSITE_MSOS_MAX          512
/** BOS 描述符缓冲上限 */
#define USB_COMPOSITE_BOS_MAX           128
/** esp_tinyusb 字符串描述符硬上限（descriptors_control.h:15）—— 已用满 8 条 */
#define USB_COMPOSITE_MAX_STR           8

/** HS 口 DWC2 数据 FIFO 总量（word，32-bit）—— frozen-allocation §2.2 */
#define USB_COMPOSITE_FIFO_WORDS_TOTAL  1024

/* =========================================================================
 * 基础类型
 * ========================================================================= */

/** 端点传输类型。本方案**只用 bulk / interrupt**，明确不提供等时（plan §1） */
typedef enum {
    USB_EP_XFER_BULK = 0,
    USB_EP_XFER_INTERRUPT = 1,
} usb_ep_xfer_t;

/**
 * @brief USB 2.0 §9.3 SETUP 包。
 *
 * 刻意**不**引 `tusb.h`，让本头文件对使用者保持轻量（与 `tusb_control_request_t` 布局一致）。
 */
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

/** 一个功能向复合层申报的端点需求 */
typedef struct {
    usb_ep_xfer_t xfer;   /**< BULK 或 INTERRUPT */
    uint16_t mps_hs;      /**< HS 的 wMaxPacketSize（bulk 512 / interrupt 64） */
    uint16_t mps_fs;      /**< FS 的 wMaxPacketSize（bulk 64 / interrupt 64） */
    uint8_t interval;     /**< 仅 interrupt 有意义；bulk 写 0 */
} usb_ep_req_t;

/**
 * @brief 复合层分配给一个功能的实际号码（回调里只读）。
 *
 * `ep_in[i]` / `ep_out[i]` 与注册时 `ep_in_req[]` / `ep_out_req[]` 的顺序一一对应。
 */
typedef struct {
    uint8_t itf[USB_COMPOSITE_MAX_ITF];           /**< 分配的接口号 */
    uint8_t itf_count;
    uint8_t ep_in[USB_COMPOSITE_MAX_EP_PER_DIR];  /**< 含方向位（0x80|n）；0 = 未使用 */
    uint8_t ep_in_count;
    uint8_t ep_out[USB_COMPOSITE_MAX_EP_PER_DIR]; /**< 不含方向位（n）；0 = 未使用 */
    uint8_t ep_out_count;
    uint8_t itf_str_idx;   /**< 首接口的字符串索引（0 = 不给接口串）；由复合层分配 */
    /**
     * 可选的第二个字符串索引（NCM 的 `iMACAddress` 用它）；0 = 未分配。
     * 只有注册时填了 `usb_function_t.extra_string` 的功能才会拿到非 0 值。
     */
    uint8_t extra_str_idx;
    uint8_t vendor_idx;    /**< 本功能在 vendor 驱动里的序号；非 vendor 功能为 0xFF */
} usb_fn_alloc_t;

/** vendor_idx 的「不适用」值 */
#define USB_FN_NO_VENDOR_IDX  0xFF

/** `ep_out_same_num_as_in[]` 的「不配对」值（独立分配 OUT 号码） */
#define USB_EP_NO_PAIR        0xFF

/* =========================================================================
 * 功能注册描述
 * ========================================================================= */

struct usb_function;

/**
 * @brief **描述符片段生成器**：把本功能的接口/端点/类描述符写成一段连续字节。
 *
 * 要求：
 *  - 输出的**第一个字节**必须是 Interface Descriptor（或 IAD，如果需要）；
 *  - 片段里**不得**包含 Configuration Descriptor（复合层负责）；
 *  - 只能使用 `alloc` 里给出的接口号/端点号/字符串索引，**不得硬编码**；
 *  - 同一功能会被调用**两次**（FS 与 HS），两次的 `alloc` 完全相同；
 *  - 返回 ESP_OK 并写 `*out_len`。`cap` 不足时返回 ESP_ERR_INVALID_SIZE。
 *
 * @param high_speed true = HS 片段，false = FS 片段（wMaxPacketSize 不同）
 * @param alloc      复合层分配的号码（只读）
 * @param buf        输出缓冲
 * @param cap        输出缓冲容量
 * @param out_len    输出长度
 */
typedef esp_err_t (*usb_fn_build_desc_t)(bool high_speed, const usb_fn_alloc_t *alloc,
                                         uint8_t *buf, size_t cap, size_t *out_len);

/**
 * @brief 功能初始化回调。在**号码已分配、TinyUSB 尚未 install** 时调用。
 *
 * 用途：创建互斥量/队列、注册内部状态、准备数据面。**不要**在这里碰 USB 硬件，
 * 也**不要**在这里读 IN 端点（端点还没打开，`usb_composite_ep_write()` 会返回 0）。
 *
 * 复用的"功能私有初始化"（如 MTP 类驱动 install）必须在 `usb_composite_start()` **之前**
 * 由 app 侧完成（MTP 有顺序要求，见 probe-contract §3.2）。
 */
typedef esp_err_t (*usb_fn_init_t)(const usb_fn_alloc_t *alloc);

/**
 * @brief OUT 端点收到数据时的通知（**数据面**）。
 *
 * 语义与 TinyUSB 的 `tud_vendor_rx_cb()` 一致：只表示"有数据可读"，
 * 功能应自行调用 `usb_composite_ep_read()` 排空，**必须一次读干净**（否则 RX FIFO 卡住）。
 * 在 TinyUSB 任务上下文调用，**不可阻塞**。
 *
 * @param ep_addr      发生事件的 OUT 端点地址（不含方向位）
 * @param user_ctx     注册时给的 user_ctx
 */
typedef void (*usb_fn_out_cb_t)(uint8_t ep_addr, void *user_ctx);

/**
 * @brief 挂载/卸载通知（可选）。设备枚举完成 / 主机断开时调用。
 *
 * ⚠️ 不要用 `tud_mount_cb()` / `tud_umount_cb()`：esp_tinyusb 2.3.0 已提供强符号，
 * 重定义会 multiple definition 链接失败（probe-contract §3.1 实测）。
 */
typedef void (*usb_fn_mount_cb_t)(bool mounted, void *user_ctx);

/**
 * @brief 供应商控制请求处理（可选）。
 *
 * 只对**指向本功能接口**的 vendor/class 控制请求调用（复合层已先处理
 * WebUSB `GET_URL` 与 MS OS 2.0 `GET_MS_DESCRIPTOR`，不会转到功能）。
 *
 * 实现约定：完全自行完成 `tud_control_xfer_cb` 的语义（`stage == SETUP` 时返回
 * true 表示"我接管了，后续 DATA/ACK 也要我处理"）。返回 false 表示不处理 → 复合层 stall。
 */
typedef bool (*usb_fn_control_cb_t)(const usb_setup_packet_t *req, void *user_ctx);

/** 一个功能的完整注册描述 */
typedef struct usb_function {
    /** 功能名（仅日志） */
    const char *name;

    /** 占用几个接口（MTP / 视频 / 控制 = 1；NCM = 2） */
    uint8_t itf_count;

    /** 首接口的字符串描述符索引需求：NULL = 不要接口串 */
    const char *itf_string;

    /**
     * 可选的第二个字符串（NCM 的 `iMACAddress` 需要："AABBCCDDEEFF" 12 个十六进制字符）。
     * NULL = 不要。分配结果在 `usb_fn_alloc_t.extra_str_idx`。
     * ⚠️ 字符串描述符总量上限 8（`USB_COMPOSITE_MAX_STR`），当前 4 个功能恰好用满；
     *    再加任何字符串都会超预算，复合层会拒绝 prepare。
     */
    const char *extra_string;

    /**
     * NCM（或任何需要 IAD 的功能）填 true：复合层会在本功能接口段之前
     * 插入 `Interface Association Descriptor`，并把 `bFirstInterfaceNumber` 填成
     * 分配到的首个接口号。
     */
    bool use_iad;

    /** 端点需求（顺序 = 分配顺序；OUT 是否复用某个 IN 的号码见 `ep_out_same_num_as_in`） */
    usb_ep_req_t ep_in_req[USB_COMPOSITE_MAX_EP_PER_DIR];
    uint8_t ep_in_count;
    usb_ep_req_t ep_out_req[USB_COMPOSITE_MAX_EP_PER_DIR];
    uint8_t ep_out_count;
    /**
     * **OUT 端点号码的显式配对表**（与 `ep_out_req[]` 同序）。
     *
     * `ep_out_same_num_as_in[j] = i` ⇒ 第 j 个 OUT 端点使用 `ep_in_req[i]` 分配到的**同一号码**；
     * `= USB_EP_NO_PAIR (0xFF)`           ⇒ 从 OUT 方向独立分配一个新号码。
     *
     * 为什么是显式索引而不是"按顺序配对"：**MTP 与 NCM 各有 2 个 IN 但只有 1 个 OUT**，
     * 而那个 OUT 必须配到**第 2 个** IN 上（MTP: EP1 事件 int + **EP2** bulk IN/OUT；
     * NCM: EP5 通知 int + **EP6** bulk IN/OUT）。任何"第 i 个配第 i 个"的隐式规则都会
     * 把它们配错成 EP1 / EP5。见 docs/contracts/frozen-allocation.md §2.1。
     *
     * 例：
     *   MTP   : ep_in=[int64, bulk512], ep_out=[bulk512], table=[1]  → OUT 拿到 EP2
     *   video : ep_in=[bulk512],       ep_out=[]                   → 无 OUT
     *   ctrl  : ep_in=[bulk512],       ep_out=[bulk512], table=[0]  → OUT 拿到 EP4
     *   NCM   : ep_in=[int16, bulk512],ep_out=[bulk512], table=[1]  → OUT 拿到 EP6
     */
    uint8_t ep_out_same_num_as_in[USB_COMPOSITE_MAX_EP_PER_DIR];

    /** 本功能是否使用 vendor 类驱动（esp_tinyusb `CONFIG_TINYUSB_VENDOR_COUNT`） */
    bool is_vendor;

    /** 描述符片段生成器（必需） */
    usb_fn_build_desc_t build_desc;
    /** 初始化（可选） */
    usb_fn_init_t init;
    /** OUT 数据通知（可选，但声明了 OUT 端点就应该实现） */
    usb_fn_out_cb_t on_out;
    /** 挂载/卸载通知（可选） */
    usb_fn_mount_cb_t on_mount;
    /** 供应商控制请求（可选） */
    usb_fn_control_cb_t on_control;
    /** 透传给上述回调 */
    void *user_ctx;

    /* ---- MS OS 2.0 / WCID（只给需要 winusb.sys 的功能填；其他保持 0） ---- */
    /** true = 复合层为本功能发 CompatibleID + Registry Property */
    bool wcid;
    /** "WINUSB"（复合层补成 `WINUSB\0\0`） */
    const char *wcid_compatible_id;
    /** 必须是 "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"（frozen-allocation §5） */
    const char *wcid_device_interface_guid;
} usb_function_t;

/* =========================================================================
 * 身份（frozen-allocation §3）
 * ========================================================================= */

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_usb;
    uint16_t bcd_device;
    uint8_t b_device_class;
    uint8_t b_device_subclass;
    uint8_t b_device_protocol;
    bool self_powered;
    /** VBUS 监测 GPIO；-1 = 不用 */
    int vbus_monitor_io;
    /** 供电声明（mA，仅描述符字段） */
    uint16_t max_power_ma;
    /* 字符串（NULL = 用冻结默认值；见 frozen-allocation §4 的索引 1/2/3） */
    const char *manufacturer;  /**< 默认 "MeidaP4" */
    const char *product;       /**< 默认 "MeidaP4 Composite" */
    const char *serial;        /**< NULL = 由实现用 eFuse MAC 生成 */
} usb_composite_identity_t;

/** 取冻结的默认身份（PID 0x4103 / bDeviceClass 0xEF/02/01 / 序列号由 eFuse MAC 生成） */
void usb_composite_identity_default(usb_composite_identity_t *out);

/**
 * @brief 覆盖身份。必须在 `usb_composite_prepare()` 之前调用。
 *
 * `bcdDevice` 是 Windows 设备实例身份的一部分：**每次改动描述符内容都必须 +1**，
 * 否则主机侧会拿到旧实例的 MS OS 2.0 注册表属性（probe-contract §2 实测）。
 */
esp_err_t usb_composite_set_identity(const usb_composite_identity_t *id);

/* =========================================================================
 * 注册与启动
 * ========================================================================= */

/**
 * @brief 注册一个功能。必须在 `usb_composite_prepare()` 之前、按希望的接口顺序调用。
 *
 * **生命周期（F9，2026-10 实测后明确）**：
 *  - 复合层会把 `*fn` **整份复制**进内部 `static` 存储 ⇒ 调用方**可以安全地传栈上对象**，
 *    只需保证 `fn` 在**本次调用返回前**有效。
 *  - ⚠️ 但**不做深拷贝**：`name` / `itf_string` / `extra_string` 指向的字符串，
 *    以及 `build_desc` / `init` / `on_out` / `on_mount` / `on_control` 指向的函数，
 *    都必须**常驻**（`static` 或字符串字面量）。传一个栈上数组当 `itf_string` 仍会悬垂。
 *  - 历史教训：NCM 曾在栈上构造 `usb_function_t` 后注册，函数返回即悬垂；
 *    `uc_prepare()` 读到被覆写的 `ep_in_count` 会误报"IN 端点耗尽"，
 *    更糟时 `build_desc` 函数指针变垃圾 → 跳随机地址。**复制结构体从根上消除了这一坑。**
 *
 * 注册顺序决定接口号与（verdor 功能的）`vendor_idx`：
 * `vendor_idx` 按**注册顺序**在 `is_vendor == true` 的功能之间从 0 递增，
 * 与 TinyUSB vendor 驱动遍历描述符的顺序一致。
 *
 * @return ESP_OK；ESP_ERR_INVALID_ARG（参数非法）；ESP_ERR_NO_MEM（超出 MAX_FUNCTIONS）；
 *         ESP_ERR_INVALID_STATE（已经 prepare 过）
 */
esp_err_t usb_composite_register(const usb_function_t *fn);

/**
 * @brief **只**组装描述符并计算预算，不安装 TinyUSB。
 *
 * 幂等（重复调用直接返回 ESP_OK）。供离线自检、单元测试与 `usb_composite_start()` 使用。
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（没有注册任何功能）；
 *         ESP_ERR_INVALID_SIZE（描述符超缓冲）；ESP_ERR_NOT_SUPPORTED（端点/FIFO/字符串超预算）
 */
esp_err_t usb_composite_prepare(void);

/**
 * @brief prepare + `tinyusb_driver_install()`。多次调用返回 ESP_ERR_INVALID_STATE。
 *
 * ⚠️ 调用方必须已经设置好 `CONFIG_TINYUSB_VENDOR_COUNT == 注册的 vendor 功能数`，
 *    且已按顺序完成各功能的"类驱动 install"（MTP：`tinyusb_mtp_install_driver()`；
 *    NCM：`tinyusb_net_init()` 由 usb_net 自己负责）。
 */
esp_err_t usb_composite_start(void);

/** 主机是否已枚举完成（来自 `tinyusb_config_t.event_cb`） */
bool usb_composite_is_mounted(void);

/* =========================================================================
 * 数据面（功能回调里用；主任务里也可用）
 * ========================================================================= */

/**
 * @brief 往自己的 IN 端点写数据（设备 → 主机）。
 *
 * 非阻塞。返回**实际接受**的字节数，可能 < len（端点/FIFO 忙）—— 调用方负责重试。
 * 返回 0 时可用 `usb_composite_ep_write_available()` 判断是"真忙"还是"端点未打开"。
 *
 * @param ep_in_addr `usb_fn_alloc_t.ep_in[i]`（含 0x80 位）
 */
size_t usb_composite_ep_write(uint8_t ep_in_addr, const void *data, size_t len);

/** 当前可写入 IN 端点的字节数（0 = 忙或未打开） */
size_t usb_composite_ep_write_available(uint8_t ep_in_addr);

/** 已排入 IN 端点、尚未发完的字节数（流控用） */
size_t usb_composite_ep_write_pending(uint8_t ep_in_addr);

/**
 * @brief 从自己的 OUT 端点读数据（主机 → 设备）。
 *
 * 在 `on_out` 回调（或主任务）里调用。**必须读到返回 0 为止**，否则 RX FIFO 会卡住。
 *
 * @param ep_out_addr `usb_fn_alloc_t.ep_out[i]`（不含方向位）
 * @return 实际读出字节数；0 = 已排空 / 端点未打开
 */
size_t usb_composite_ep_read(uint8_t ep_out_addr, void *buf, size_t cap);

/** 当前 OUT 端点可读字节数 */
size_t usb_composite_ep_read_available(uint8_t ep_out_addr);

/* =========================================================================
 * 自检 / 离线校验（verifier 与启动期都会用）
 * ========================================================================= */

/** 端点与 FIFO 预算（frozen-allocation §2.2 的实算结果） */
typedef struct {
    uint8_t itf_total;
    uint8_t fn_total;
    uint8_t vendor_fn_total;
    uint8_t ep_in_total;    /**< 用户 IN，不含 EP0 */
    uint8_t ep_out_total;
    uint16_t fifo_words_used;
    uint16_t fifo_words_total;
    uint16_t fifo_words_free;
    uint32_t fs_cfg_len;
    uint32_t hs_cfg_len;
    uint32_t bos_len;
    uint32_t msos_len;
    uint32_t str_count;
} usb_composite_budget_t;

/** 取预算（内部会先隐式 prepare）。任一参数为 NULL 时安全返回 */
esp_err_t usb_composite_get_budget(usb_composite_budget_t *out);

/**
 * @brief **离线自检**：逐字节解析自己组装出来的描述符，并与冻结表逐项比对。
 *
 * 检查项（失败即 `ESP_ERR_INVALID_STATE`，并逐条 `ESP_LOGE` 说明）：
 *  - 配置描述符 `wTotalLength` / `bNumInterfaces` / `bConfigurationValue`
 *  - 每个接口的类/子类/协议、接口号、`iInterface`
 *  - 每个端点的地址/类型/`wMaxPacketSize`（**按速度分别校验 FS 64 / HS 512**）
 *  - `bcdUSB >= 0x0210`、BOS 平台能力数量与 UUID
 *  - MS OS 2.0 描述符集总长度自洽 + Registry Property 必须是**单数** GUID + `REG_SZ`
 *  - 端点号不重复、方向合法、IN 总数 ≤ 7
 *  - DWC2 FIFO 预算 (`fifo_words_used <= USB_COMPOSITE_FIFO_WORDS_TOTAL`)
 *
 * 这是**离线自检**，不替代真机枚举。
 */
esp_err_t usb_composite_selfcheck(void);

/** 取组装好的描述符（供主机侧/离线工具 dump）。`*len` 可为 NULL */
const uint8_t *usb_composite_config_desc(bool high_speed, uint32_t *len);
const uint8_t *usb_composite_bos_desc(uint32_t *len);
const uint8_t *usb_composite_msos_desc(uint32_t *len);
const uint8_t *usb_composite_device_desc(uint32_t *len);

/* =========================================================================
 * 统计
 * ========================================================================= */

typedef struct {
    bool mounted;
    uint32_t mount_count;
    uint32_t unmount_count;
    uint32_t ctrl_requests;   /**< 收到的供应商控制请求总数 */
    uint32_t ctrl_stalls;     /**< 未处理而 stall 的数量 */
    uint32_t webusb_get_url;  /**< WebUSB GET_URL 次数（应恒为 0：iLandingPage=0） */
    uint32_t msos_requests;   /**< MS OS 2.0 GET_MS_DESCRIPTOR 次数 */
} usb_composite_stats_t;

void usb_composite_get_stats(usb_composite_stats_t *out);

#ifdef __cplusplus
}
#endif
