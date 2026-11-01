// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 探测阶段 MTP 后端
//
// 职责（docs/probe-contract.md §3.2）：
//   1. 把 partitions.csv 里的 `storage`（data/fat，4 MB @0x410000）用 wear_levelling
//      挂成 rw FATFS，挂载点 = CONFIG_PROBE_MTP_MOUNT_POINT（默认 "/mtp"）。
//   2. 没找到 FATFS 时按 CONFIG_PROBE_MTP_FORMAT_IF_NEEDED 格式化一次。
//   3. 往卷里放一个 PROBE.txt（IDF 版本 + 时间戳），让主机 MTP 有东西可看。
//   4. tinyusb_mtp_install_driver() + tinyusb_mtp_register_storage()。
//
// ⚠️ 调用顺序：main/main.cpp 保证 mtp_probe_start() 在 usb_probe_start() 之前，
//    MTP 类驱动必须先把 glue 装好，tinyusb_driver_install() 才能带上 MTP 接口。
// ⚠️ 探测期只用片内 storage 分区，不碰 SD 卡（LDO4/引脚风险留到完整版）。
//
// 生产者：mtp-core。写作用域：components/mtp_probe/**。

#include "sdkconfig.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "mtp_probe.h"

#if CONFIG_TINYUSB_MTP_ENABLED
#include "tinyusb_mtp.h"
#endif

// ---------------------------------------------------------------------------
// 配置兜底：正常由 main/Kconfig.projbuild 提供；独立构建本项目时也不至于编不过。
// ---------------------------------------------------------------------------
#ifndef CONFIG_PROBE_MTP_MOUNT_POINT
#define CONFIG_PROBE_MTP_MOUNT_POINT "/mtp"
#endif
#ifndef CONFIG_PROBE_MTP_FORMAT_IF_NEEDED
#define CONFIG_PROBE_MTP_FORMAT_IF_NEEDED 1
#endif

#define MTP_PROBE_PARTITION_LABEL  "storage"
#define MTP_PROBE_MARKER_FILE      "PROBE.txt"
#define MTP_PROBE_MAX_OPEN_FILES   5
// allocation unit 必须落在 [WL 扇区, 128*WL 扇区] 内。本工程
// CONFIG_WL_SECTOR_SIZE=4096（默认）且 CONFIG_FATFS_SECTOR_4096=y，故取 4096：
// 1 个 FATFS 簇 == 1 个 WL 扇区，写入时由 ff_wl_write() 整扇区 erase+write。
#define MTP_PROBE_ALLOC_UNIT       4096
#define MTP_PROBE_VERSION_STR      "0.1.0-probe"

static const char *TAG = "mtp_probe";

// 卷是否挂载成功（与 MTP 注册是否成功无关，见头文件语义）。
static bool s_mounted = false;

#if CONFIG_TINYUSB_MTP_ENABLED

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static tinyusb_mtp_storage_handle_t s_storage = NULL;

static const esp_vfs_fat_mount_config_t s_mount_cfg = {
    // 故意用 false：格式化走下面显式分支，日志里才能明确说"这个卷被格式化过"。
    .format_if_mount_failed = false,
    .max_files = MTP_PROBE_MAX_OPEN_FILES,
    .allocation_unit_size = MTP_PROBE_ALLOC_UNIT,
    .disk_status_check_enable = false,
    .use_one_fat = false,
};

/**
 * @brief 挂载 storage 分区为 rw FATFS-on-wear_levelling；必要时格式化一次。
 *
 * 用的是 IDF 6.1 仍然存在的经典 API（未废弃）：
 *   esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path,
 *                                              const char *partition_label,
 *                                              const esp_vfs_fat_mount_config_t *mount_config,
 *                                              wl_handle_t *wl_handle);
 *   esp_err_t esp_vfs_fat_spiflash_format_rw_wl(const char *base_path,
 *                                               const char *partition_label);
 * （原型见 C:\esp\v6.1\esp-idf\components\fatfs\vfs\esp_vfs_fat.h:314 / :368）
 */
static esp_err_t mtp_probe_mount_volume(void)
{
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(CONFIG_PROBE_MTP_MOUNT_POINT,
                                                    MTP_PROBE_PARTITION_LABEL,
                                                    &s_mount_cfg, &s_wl_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted partition '%s' (data/fat) at '%s' [rw + wear_levelling]",
                 MTP_PROBE_PARTITION_LABEL, CONFIG_PROBE_MTP_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "mount '%s' -> '%s' failed: %s (0x%x)", MTP_PROBE_PARTITION_LABEL,
             CONFIG_PROBE_MTP_MOUNT_POINT, esp_err_to_name(err), (unsigned)err);

    if (err == ESP_ERR_NOT_FOUND) {
        // 分区表里没有 storage：格式化也救不了，直接报出去（partitions.csv / 烧录问题）。
        ESP_LOGE(TAG, "partition '%s' (type=data subtype=fat) not found in the flashed partition table",
                 MTP_PROBE_PARTITION_LABEL);
        return err;
    }

#if CONFIG_PROBE_MTP_FORMAT_IF_NEEDED
    // 首次上电 / 分区里不是 FATFS：format_rw_wl() 内部会先 mount（format_if_mount_failed=true）
    // 再 mkfs 再恢复原状；返回后分区已是"格式化好且未挂载"状态，需要再挂一次。
    ESP_LOGW(TAG, "no usable FATFS on '%s' -> FORMATTING it (CONFIG_PROBE_MTP_FORMAT_IF_NEEDED=y)",
             MTP_PROBE_PARTITION_LABEL);
    err = esp_vfs_fat_spiflash_format_rw_wl(CONFIG_PROBE_MTP_MOUNT_POINT, MTP_PROBE_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_spiflash_format_rw_wl('%s') failed: %s (0x%x)",
                 MTP_PROBE_PARTITION_LABEL, esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGW(TAG, "partition '%s' was FORMATTED during this boot (no FATFS found before)",
             MTP_PROBE_PARTITION_LABEL);

    // format_rw_wl() 自己 mount/unmount 过，句柄不复用（其内部句柄已 wl_unmount）。
    s_wl_handle = WL_INVALID_HANDLE;
    err = esp_vfs_fat_spiflash_mount_rw_wl(CONFIG_PROBE_MTP_MOUNT_POINT,
                                          MTP_PROBE_PARTITION_LABEL,
                                          &s_mount_cfg, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "re-mount after format failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGI(TAG, "mounted partition '%s' after format at '%s' [rw + wear_levelling]",
             MTP_PROBE_PARTITION_LABEL, CONFIG_PROBE_MTP_MOUNT_POINT);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "CONFIG_PROBE_MTP_FORMAT_IF_NEEDED=n -> not formatting, MTP volume stays unavailable");
    return err;
#endif
}

/**
 * @brief 往卷里写 PROBE.txt（每次启动重写，时间戳即"本次启动时间"）。
 *
 * 用 ASCII 文件名 + ASCII 内容：探测期不引入任何编码变量（UTF-8 文件名的端到端
 * 验证交给主机侧 MTP 写文件那一步）。
 * 写失败不是致命错误（MTP 存储照样能注册），但要打 ERROR。
 */
static void mtp_probe_write_marker(void)
{
    char path[64];
    int n = snprintf(path, sizeof(path), "%s/%s", CONFIG_PROBE_MTP_MOUNT_POINT, MTP_PROBE_MARKER_FILE);
    if (n <= 0 || n >= (int)sizeof(path)) {
        ESP_LOGE(TAG, "marker path truncated");
        return;
    }

    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_ms = (uint32_t)(uptime_us / 1000);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot create %s (volume is mounted rw but open failed) - MTP storage still usable", path);
        return;
    }
    fprintf(f, "MeidaP4 MTP probe volume\n");
    fprintf(f, "========================\n");
    fprintf(f, "file        : %s\n", MTP_PROBE_MARKER_FILE);
    fprintf(f, "idf version : %s\n", esp_get_idf_version());
    fprintf(f, "firmware    : %s\n", MTP_PROBE_VERSION_STR);
    fprintf(f, "build stamp : %s %s\n", __DATE__, __TIME__);
    fprintf(f, "boot time   : %" PRIu32 " ms after reset\n", uptime_ms);
    fprintf(f, "mount point : %s\n", CONFIG_PROBE_MTP_MOUNT_POINT);
    fprintf(f, "backing     : internal flash partition '%s' (data/fat, wear_levelling, read-write)\n",
            MTP_PROBE_PARTITION_LABEL);
    fprintf(f, "\n");
    fprintf(f, "If you can read this from the host through MTP, the MTP + vendor\n");
    fprintf(f, "composite enumerated and the file-level storage works.\n");
    fprintf(f, "Writing a new file here from the host exercises the rw mount path.\n");
    fflush(f);
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "fclose(%s) failed", path);
        return;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "marker written: %s (%ld bytes)", path, (long)st.st_size);
    } else {
        ESP_LOGW(TAG, "marker written but stat(%s) failed", path);
    }
}

#endif /* CONFIG_TINYUSB_MTP_ENABLED */

esp_err_t mtp_probe_start(void)
{
#if !CONFIG_TINYUSB_MTP_ENABLED
    // 模式 A：esp_tinyusb 没编 MTP 类，FATFS 卷也不需要挂——整条后端直接不提供。
    ESP_LOGW(TAG, "CONFIG_TINYUSB_MTP_ENABLED=n: MTP probe backend not built (mode A)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mounted) {
        ESP_LOGW(TAG, "volume already mounted at '%s'", CONFIG_PROBE_MTP_MOUNT_POINT);
        return ESP_OK;
    }

    esp_err_t err = mtp_probe_mount_volume();
    if (err != ESP_OK) {
        return err;
    }
    s_mounted = true;

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(CONFIG_PROBE_MTP_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        // 不做浮点格式化（省 newlib 体积）：字节 + KiB。
        ESP_LOGI(TAG, "volume '%s': total=%" PRIu64 " B (%" PRIu64 " KiB), free=%" PRIu64 " B (%" PRIu64 " KiB)",
                 CONFIG_PROBE_MTP_MOUNT_POINT, total, total / 1024, free_bytes, free_bytes / 1024);
    } else {
        ESP_LOGW(TAG, "esp_vfs_fat_info('%s') failed", CONFIG_PROBE_MTP_MOUNT_POINT);
    }

    // 先放文件、再注册：主机枚举到 MTP 设备时卷里已经有东西可看。
    mtp_probe_write_marker();

    const tinyusb_mtp_driver_config_t driver_cfg = {
        .manufacturer = "MeidaP4",
        .model = "MeidaP4 Probe",
        .version = MTP_PROBE_VERSION_STR,
        .serial = "PROBE-0001",
        .friendly_name = "MeidaP4 Storage",
    };
    err = tinyusb_mtp_install_driver(&driver_cfg);
    if (err != ESP_OK) {
        // ESP_ERR_INVALID_STATE = 已经装过；其余（NO_MEM）也不该继续注册。
        ESP_LOGE(TAG, "tinyusb_mtp_install_driver() failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGI(TAG, "tinyusb MTP class driver installed (strings: MeidaP4 / MeidaP4 Probe / %s / PROBE-0001)",
             MTP_PROBE_VERSION_STR);

    const tinyusb_mtp_storage_config_t storage_cfg = {
        .base_path = CONFIG_PROBE_MTP_MOUNT_POINT,
        .display_name = "MeidaP4 Storage",
        .removable = true,
    };
    err = tinyusb_mtp_register_storage(&storage_cfg, &s_storage);
    if (err != ESP_OK) {
        // ESP_ERR_NOT_FOUND = 这个路径不是已挂载的 FATFS（内部会做 esp_vfs_fat_info 校验）。
        ESP_LOGE(TAG, "tinyusb_mtp_register_storage('%s') failed: %s (0x%x) - volume stays mounted, MTP has no storage",
                 CONFIG_PROBE_MTP_MOUNT_POINT, esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGI(TAG, "MTP storage registered: path='%s' display_name='MeidaP4 Storage' removable=1",
             CONFIG_PROBE_MTP_MOUNT_POINT);
    return ESP_OK;
#endif
}

bool mtp_probe_is_mounted(void)
{
    return s_mounted;
}

const char *mtp_probe_mount_point(void)
{
    return CONFIG_PROBE_MTP_MOUNT_POINT;
}
