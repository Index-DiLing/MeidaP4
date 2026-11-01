// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · SD 卡存储卷（WS-storage）—— Sdmmc slot 0 / 4-bit / 40 MHz / 片内 LDO4 供电 / FATFS
//
//   生产者：WS-storage。写作用域：components/sd_storage/**
//   消费者：components/mtp_backend（把本卷注册成 MTP 唯一卷）
//
// 设计要点：
//   * **挂载点只有一处定义**（`CONFIG_SD_STORAGE_MOUNT_POINT`）。mtp_backend 用
//     `sd_storage_mount_point()` 取路径，自己没有第二份配置 —— "FATFS 挂在哪"与
//     "MTP 注册哪个路径"编译期就不可能不一致。
//   * **挂载失败绝不 abort**：本组件只返回错误码 + 打日志。探测期已确认（probe-report §2.3），
//     存储半边失败时 USB 那一半必须继续跑，否则连诊断都做不了。
//   * SD 卡供电由 **esp_ldo_regulator**（片内 LDO，默认通道 4）负责：漏了这步卡不会工作
//     （plan §4.4）。同时把这条 LDO 包成一个 `sd_pwr_ctrl_drv_t` 交给 SDMMC 驱动，
//     这样 UHS-I 卡的 CMD11 调压（1.8 V）路径也是通的（否则该回调会因句柄为空而失败）。
//
// 未验证项（设备未连接，全部离线）：LDO4 实际输出电压、引脚、上电时序、真机卡识别。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 卡产品名（CID name）最长 8 字符 + NUL */
#define SD_STORAGE_CARD_NAME_MAX 16

/** 一次挂载后的卷状态快照（给 mtp_backend / 日志 / 主机侧遥测用） */
typedef struct {
    bool        mounted;         /**< FATFS 卷是否已挂载 */
    const char *mount_point;     /**< 与 CONFIG_SD_STORAGE_MOUNT_POINT 同值 */
    uint64_t    total_bytes;     /**< 卷总容量（esp_vfs_fat_info；未挂载为 0） */
    uint64_t    free_bytes;      /**< 卷可用容量（未挂载为 0） */
    int         max_freq_khz;    /**< 配置的 SDMMC 时钟 */
    int         real_freq_khz;   /**< 驱动实际跑到的时钟（0 = 取不到） */
    bool        ldo_used;        /**< 是否用了片内 LDO 供电 */
    int         ldo_channel;     /**< 片内 LDO 通道号（1-based） */
    int         ldo_voltage_mv;  /**< acquire 时设置的电压 */
    char        card_name[SD_STORAGE_CARD_NAME_MAX]; /**< CID name，取不到则为空串 */
} sd_storage_info_t;

/**
 * @brief 给 SD 卡上电（片内 LDO）并把卡挂成 FATFS。**幂等**。
 *
 * 成功：返回 ESP_OK，`sd_storage_is_mounted() == true`。
 * 失败：返回底层错误码（ESP_FAIL = 卡在但无 FATFS 且不允许格式化；ESP_ERR_TIMEOUT =
 *       卡无响应/不在），**卷保持未挂载**，调用方必须继续往下走（不要 abort）。
 *
 * 注意：必须在 USB / MTP 起来之前调用（MTP 需要路径已经是已挂载的 FATFS）。
 */
esp_err_t sd_storage_init(void);

/**
 * @brief 卸载卷并释放片内 LDO 通道。
 *
 * ⚠️ 只在**没有 MTP 存储注册**时调用：MTP 注册的 base_path 一旦消失，主机的
 *    文件操作会全部失败。正常固件不需要它。
 */
esp_err_t sd_storage_deinit(void);

/** 卷是否已挂载（唯一可靠的是否可用判据） */
bool sd_storage_is_mounted(void);

/**
 * @brief 取挂载点。**永远非 NULL**（未挂载时也返回配置值）。
 *
 * mtp_backend 必须用这个函数取路径，不要在别处再写一遍字面量。
 */
const char *sd_storage_mount_point(void);

/**
 * @brief 取卷容量。
 * @return ESP_OK；ESP_ERR_INVALID_STATE（未挂载）；ESP_ERR_INVALID_ARG（out 为空）
 */
esp_err_t sd_storage_get_capacity(uint64_t *total_bytes, uint64_t *free_bytes);

/**
 * @brief 取完整卷状态快照。
 *
 * 未挂载时**仍然返回 ESP_OK**（`out->mounted == false`，容量为 0）—— 让调用方
 * 一条日志就能打印"存储现在是什么状态"，不必先分支。
 *
 * @return ESP_OK；ESP_ERR_INVALID_ARG
 */
esp_err_t sd_storage_get_info(sd_storage_info_t *out);

#ifdef __cplusplus
}
#endif
