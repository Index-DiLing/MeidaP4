// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · MTP 后端（WS-storage）—— 把 SD 卡注册成**唯一** MTP 卷，并向 I1 复合层注册 MTP 功能
//
//   生产者：WS-storage。写作用域：components/mtp_backend/**
//   依赖契约：components/usb_composite/include/usb_composite.h（I1）
//             docs/contracts/frozen-allocation.md（IF0 = MTP / EP1 int IN 64 / EP2 bulk IN+OUT 512）
//
// ⚠️ 调用顺序（freeze：probe-contract §3.2 + frozen-allocation §1）：
//     main 必须**第一个**调用 mtp_backend_start()，然后才调 usb_composite_start()：
//       1) MTP 类驱动（tinyusb_mtp_install_driver）必须先 install，TinyUSB 才能把 MTP 接口挂上；
//       2) `usb_composite_register()` 的顺序决定接口号 —— MTP 要拿到 IF0，就必须先注册。
//     本函数内部**不**碰 usb_composite_start()：复合层的安装时机由 main 掌握。
//
// 设备未连接，本组件的一切结论都只到"编译 + 静态检查"为止（见完成报告"未验证项"）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 MTP 后端：确保 SD 卷可用 → install MTP 类驱动 → 注册 SD 卷 → 注册 I1 功能。
 *
 * **幂等**：重复调用直接返回 ESP_OK（不重复 install / 注册）。
 *
 * 返回值语义（重要）：
 *   - ESP_OK                  ：MTP 类驱动已 install **且** I1 功能已注册。
 *     此时存储可能**仍然没有**注册（SD 卡没挂上）—— 用 mtp_backend_is_storage_registered() 判断。
 *   - ESP_ERR_NOT_SUPPORTED   ：CONFIG_TINYUSB_MTP_ENABLED=n，本固件里没有 MTP。
 *   - 其它错误码              ：MTP 驱动 install 或 I1 注册失败（日志里有细节）。
 *
 * ⚠️ **无论返回什么，调用方都不要 abort**：存储/ MTP 失败时 USB 那一半仍必须能跑
 *    （探测期结论，probe-report §2.3）。
 */
esp_err_t mtp_backend_start(void);

/** MTP 类驱动是否已 install（tinyusb_mtp_install_driver 成功） */
bool mtp_backend_is_driver_installed(void);

/** SD 卡是否已作为 MTP 存储注册成功（= 主机能看到一个可浏览的卷） */
bool mtp_backend_is_storage_registered(void);

/**
 * @brief 返回 MTP 卷的 base_path。
 *
 * **恒等于 `sd_storage_mount_point()`**（同源，编译期就一致）；MTP 未注册时也返回该路径。
 */
const char *mtp_backend_mount_point(void);

/** 主机可见的存储/设备名（CONFIG_MTP_BACKEND_STORAGE_NAME） */
const char *mtp_backend_storage_name(void);

/**
 * @brief 离线自检：运行时生成的 MTP 描述符片段是否与 TinyUSB 自己的
 *        `TUD_MTP_DESCRIPTOR` 模板**逐字节相同**（FS 与 HS 各一次）。
 *
 * 这是把"我拼的描述符"钉死在"上游/tinyusb 认可的形态"上的机械证据；
 * `mtp_backend_start()` 里会调用一次并打印结果。纯计算，不碰硬件。
 */
bool mtp_backend_desc_selfcheck(void);

#ifdef __cplusplus
}
#endif
