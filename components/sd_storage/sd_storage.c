// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · SD 卡存储卷（WS-storage）
//
// 职责（plan §4.4 / docs/contracts/frozen-allocation.md）：
//   1. 用 **esp_ldo_regulator** 给 SD 卡供电（片内 LDO，默认通道 4 = LDO_VO4）。
//      ⚠️ plan §4.4 明确点名：**漏了这步 SD 卡不会工作**，这是最容易踩的坑。
//   2. SDMMC **slot 0** / **4-bit** / `SDMMC_FREQ_HIGHSPEED` (40 MHz) /
//      `SDMMC_SLOT_FLAG_INTERNAL_PULLUP`，挂成 rw FATFS（默认 `/sdcard`）。
//   3. 首次没有可用 FATFS 时按 Kconfig 格式化一次；**挂载失败绝不 abort**
//      （探测期已确认：存储失败时 USB 那一半仍必须能跑，见 probe-report §2.3）。
//   4. 把挂载点/挂载状态/容量暴露给 mtp_backend。
//
// 生产者：WS-storage。写作用域：components/sd_storage/**。
//
// ⚠️ 未验证（设备未连接）：LDO4 实际输出电压、引脚定义、上电时序、真机卡识别。
//    本文件里所有电气与引脚取值都来自 plan §4.4 + hello_p4/main/main.cpp 的注释
//    （"SD 卡：SDMMC slot 0，4 位，片内 LDO4 供电"）与 hello_p4/main/main_sd_card.cpp.back。

#include "sdkconfig.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "driver/sdmmc_host.h"   // SDMMC_HOST_DEFAULT() / sdmmc_slot_config_t
#include "esp_ldo_regulator.h"   // esp_ldo_acquire_channel()  ← plan §4.4 点名的供电步骤
#include "esp_vfs_fat.h"         // esp_vfs_fat_sdmmc_mount() / esp_vfs_fat_info()
#include "sd_pwr_ctrl.h"         // sd_pwr_ctrl_handle_t
#include "sd_pwr_ctrl_interface.h"
#include "sdmmc_cmd.h"           // sdmmc_card_t / sdmmc_card_print_info()

#include "sd_storage.h"

// ---------------------------------------------------------------------------
// Kconfig 兜底
//
// ⚠️ 只给"字符串/整数"兜底：这两类 Kconfig 项**永远**会被写进 sdkconfig.h。
//    **布尔项绝不能 #ifndef 兜底** —— Kconfig 里显式选 n 的布尔在 sdkconfig.h 里
//    是"完全没有定义"（不是定义为 0），兜底会把用户的 n 悄悄变成 y。
//    所以布尔项一律直接写 `#if CONFIG_X`（未定义 = 0 = 关）。
// ---------------------------------------------------------------------------
#ifndef CONFIG_SD_STORAGE_MOUNT_POINT
#define CONFIG_SD_STORAGE_MOUNT_POINT "/sdcard"
#endif
#ifndef CONFIG_SD_STORAGE_SDMMC_SLOT
#define CONFIG_SD_STORAGE_SDMMC_SLOT 0
#endif
#ifndef CONFIG_SD_STORAGE_BUS_WIDTH
#define CONFIG_SD_STORAGE_BUS_WIDTH 4
#endif
#ifndef CONFIG_SD_STORAGE_MAX_FREQ_KHZ
#define CONFIG_SD_STORAGE_MAX_FREQ_KHZ 40000   // SDMMC_FREQ_HIGHSPEED
#endif
#ifndef CONFIG_SD_STORAGE_PIN_CLK
#define CONFIG_SD_STORAGE_PIN_CLK 43
#endif
#ifndef CONFIG_SD_STORAGE_PIN_CMD
#define CONFIG_SD_STORAGE_PIN_CMD 44
#endif
#ifndef CONFIG_SD_STORAGE_PIN_D0
#define CONFIG_SD_STORAGE_PIN_D0 39
#endif
#ifndef CONFIG_SD_STORAGE_PIN_D1
#define CONFIG_SD_STORAGE_PIN_D1 40
#endif
#ifndef CONFIG_SD_STORAGE_PIN_D2
#define CONFIG_SD_STORAGE_PIN_D2 41
#endif
#ifndef CONFIG_SD_STORAGE_PIN_D3
#define CONFIG_SD_STORAGE_PIN_D3 42
#endif
#ifndef CONFIG_SD_STORAGE_LDO_CHANNEL
#define CONFIG_SD_STORAGE_LDO_CHANNEL 4
#endif
#ifndef CONFIG_SD_STORAGE_LDO_VOLTAGE_MV
#define CONFIG_SD_STORAGE_LDO_VOLTAGE_MV 3300
#endif
#ifndef CONFIG_SD_STORAGE_LDO_STABLE_DELAY_US
#define CONFIG_SD_STORAGE_LDO_STABLE_DELAY_US 1000
#endif
#ifndef CONFIG_SD_STORAGE_MAX_FILES
#define CONFIG_SD_STORAGE_MAX_FILES 16
#endif
#ifndef CONFIG_SD_STORAGE_ALLOC_UNIT_SIZE
#define CONFIG_SD_STORAGE_ALLOC_UNIT_SIZE 16384
#endif

#define SD_STORAGE_TAG "sd_storage"

/** 卡根目录里的信息文件（真机验收的"P4 侧能 fopen 主机正在浏览的那个文件"抓手） */
#define SD_STORAGE_INFO_FILE "MeidaP4-SD-INFO.txt"

/** 只用于日志的文字，不去碰冻结的分配表 */
#define SD_STORAGE_FW_NAME "MeidaP4 SD storage"

static const char *TAG = SD_STORAGE_TAG;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static sdmmc_card_t *s_card = NULL;
static bool          s_mounted = false;

#if CONFIG_SD_STORAGE_LDO_ENABLE
static esp_ldo_channel_handle_t s_ldo = NULL;

/**
 * @brief SDMMC 驱动要求的"改 IO 电压"回调 —— 直接落到片内 LDO。
 *
 * 为什么必须给 `host.pwr_ctrl_handle` 而不是只 acquire LDO 就完事：
 * `sdmmc_init()` 一上来就用 `host.io_voltage`(=3.3V) 调这个回调，而 UHS-I 卡
 * （ocr 里有 S18R 的卡相当多）还会走 `sdmmc_init_sd_uhs1()` 的 CMD11 调压路径；
 * 句柄为空时 `sd_pwr_ctrl_set_io_voltage(NULL, …)` 直接返回 ESP_ERR_INVALID_ARG，
 * 那条路径就断了。用我们自己的 LDO 句柄实现这两个回调最直接。
 */
static esp_err_t sd_storage_ldo_set_io_voltage(void *ctx, int voltage_mv)
{
    (void)ctx;
    if (s_ldo == NULL) {
        ESP_LOGE(TAG, "IO voltage switch requested (%d mV) but the on-chip LDO channel is not held", voltage_mv);
        return ESP_ERR_INVALID_STATE;
    }
    if (voltage_mv <= 0) {
        ESP_LOGE(TAG, "IO voltage switch requested with an invalid value (%d mV)", voltage_mv);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_ldo_channel_adjust_voltage(s_ldo, voltage_mv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ldo_channel_adjust_voltage(%d mV) failed: %s (0x%x)",
                 voltage_mv, esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGI(TAG, "SD IO voltage now %d mV (on-chip LDO channel %d)",
             voltage_mv, CONFIG_SD_STORAGE_LDO_CHANNEL);
    return ESP_OK;
}

static sd_pwr_ctrl_drv_t s_pwr_ctrl = {
    .set_io_voltage = sd_storage_ldo_set_io_voltage,
    .ctx = NULL,
};
#endif /* CONFIG_SD_STORAGE_LDO_ENABLE */

/**
 * @brief 打开 SD 卡供电：acquire 片内 LDO 通道（幂等）。
 *
 * ESP32-P4 的 LDO 通道号是 1-based（1 = LDO_VO1 … 4 = LDO_VO4），四个通道都可调压。
 */
static esp_err_t sd_storage_power_on(void)
{
#if CONFIG_SD_STORAGE_LDO_ENABLE
    if (s_ldo != NULL) {
        return ESP_OK;
    }

    const esp_ldo_channel_config_t cfg = {
        .chan_id = CONFIG_SD_STORAGE_LDO_CHANNEL,
        .voltage_mv = CONFIG_SD_STORAGE_LDO_VOLTAGE_MV,
        .voltage_stable_delay_us = CONFIG_SD_STORAGE_LDO_STABLE_DELAY_US,
        .flags.adjustable = 1,   // 允许后续按 UHS-I/3.3V 需要调压
    };

    esp_err_t err = esp_ldo_acquire_channel(&cfg, &s_ldo);
    if (err != ESP_OK) {
        s_ldo = NULL;
        ESP_LOGE(TAG, "esp_ldo_acquire_channel(chan=%d, %d mV) failed: %s (0x%x)",
                 CONFIG_SD_STORAGE_LDO_CHANNEL, CONFIG_SD_STORAGE_LDO_VOLTAGE_MV,
                 esp_err_to_name(err), (unsigned)err);
        ESP_LOGE(TAG, "  → 本板的 SD 卡由片内 LDO 供电，供电没起来卡一定读不到；"
                      "若这块板是外部稳压供电，请关掉 CONFIG_SD_STORAGE_LDO_ENABLE");
        return err;
    }

    ESP_LOGI(TAG, "on-chip LDO channel %d enabled: %d mV, stable delay %d us (SD card VDD)",
             CONFIG_SD_STORAGE_LDO_CHANNEL, CONFIG_SD_STORAGE_LDO_VOLTAGE_MV,
             CONFIG_SD_STORAGE_LDO_STABLE_DELAY_US);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "CONFIG_SD_STORAGE_LDO_ENABLE=n: assuming the SD card is powered externally");
    return ESP_OK;
#endif
}

/** 挂载成功后往卡根目录写一个信息文件（内容即"设备侧确实写进了这张卡"的证据） */
static void sd_storage_write_info_file(void)
{
#if CONFIG_SD_STORAGE_WRITE_INFO_FILE
    char path[96];
    int n = snprintf(path, sizeof(path), "%s/%s", CONFIG_SD_STORAGE_MOUNT_POINT, SD_STORAGE_INFO_FILE);
    if (n <= 0 || n >= (int)sizeof(path)) {
        ESP_LOGE(TAG, "info file path truncated");
        return;
    }

    uint64_t total = 0, free_bytes = 0;
    (void)esp_vfs_fat_info(CONFIG_SD_STORAGE_MOUNT_POINT, &total, &free_bytes);

#if CONFIG_SD_STORAGE_LDO_ENABLE
    const char *ldo_note = " (on-chip LDO)";
#else
    const char *ldo_note = " (LDO path disabled - external supply assumed)";
#endif

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot create %s - volume is mounted rw but open failed "
                      "(MTP still works, this is only the boot marker)", path);
        return;
    }
    fprintf(f, "%s\n", SD_STORAGE_FW_NAME);
    fprintf(f, "==============================\n");
    fprintf(f, "mount point : %s\n", CONFIG_SD_STORAGE_MOUNT_POINT);
    fprintf(f, "idf version : %s\n", esp_get_idf_version());
    fprintf(f, "build stamp : %s %s\n", __DATE__, __TIME__);
    fprintf(f, "uptime      : %" PRIu32 " ms after reset\n", (uint32_t)(esp_timer_get_time() / 1000));
    fprintf(f, "sdmmc slot  : %d (%d-bit, %d kHz requested)\n", CONFIG_SD_STORAGE_SDMMC_SLOT,
            CONFIG_SD_STORAGE_BUS_WIDTH, CONFIG_SD_STORAGE_MAX_FREQ_KHZ);
    fprintf(f, "capacity    : total=%" PRIu64 " B, free=%" PRIu64 " B\n", total, free_bytes);
    fprintf(f, "power       : on-chip LDO channel %d @ %d mV%s\n", CONFIG_SD_STORAGE_LDO_CHANNEL,
            CONFIG_SD_STORAGE_LDO_VOLTAGE_MV, ldo_note);
    fprintf(f, "\n");
    fprintf(f, "This file is written by the ESP32-P4 at boot. If you can read it over MTP,\n");
    fprintf(f, "the MTP volume really is this SD card, and the device can write to the card\n");
    fprintf(f, "while the host is browsing the same volume (file-level sharing, not exclusive).\n");
    fflush(f);
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "fclose(%s) failed", path);
        return;
    }
    ESP_LOGI(TAG, "boot marker written: %s", path);
#endif /* CONFIG_SD_STORAGE_WRITE_INFO_FILE */
}

/**
 * @brief 真正的挂载：先按"卡上是已有 FATFS"挂一次，失败且是 ESP_FAIL
 *        （= 卡在、但读不到文件系统）才按 Kconfig 决定要不要格式化。
 *
 * 为什么分两步而不是直接 `format_if_mount_failed = true`：
 *   ① 日志能明确区分"挂上了现成的 FATFS"与"这次开机把它格式化了"（探测期同款做法）；
 *   ② **卡不在时不做无谓的第二次卡初始化**（拔卡时白等一轮超时）。
 *      IDF 的 s_f_mount() 只在 FR_NO_FILESYSTEM / FR_INT_ERR 时才格式化成 ESP_FAIL，
 *      卡级错误（超时/无响应）走的是别的错误码。
 */
static esp_err_t sd_storage_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = CONFIG_SD_STORAGE_SDMMC_SLOT;
    host.max_freq_khz = CONFIG_SD_STORAGE_MAX_FREQ_KHZ;
#if CONFIG_SD_STORAGE_LDO_ENABLE
    host.pwr_ctrl_handle = (s_ldo != NULL) ? &s_pwr_ctrl : NULL;
#else
    host.pwr_ctrl_handle = NULL;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = CONFIG_SD_STORAGE_BUS_WIDTH;
    slot.clk = (gpio_num_t)CONFIG_SD_STORAGE_PIN_CLK;
    slot.cmd = (gpio_num_t)CONFIG_SD_STORAGE_PIN_CMD;
    slot.d0 = (gpio_num_t)CONFIG_SD_STORAGE_PIN_D0;
    slot.d1 = (gpio_num_t)CONFIG_SD_STORAGE_PIN_D1;
    slot.d2 = (gpio_num_t)CONFIG_SD_STORAGE_PIN_D2;
    slot.d3 = (gpio_num_t)CONFIG_SD_STORAGE_PIN_D3;
#if CONFIG_SD_STORAGE_INTERNAL_PULLUP
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   // 见本函数注释：格式化走下面的显式分支
        .max_files = CONFIG_SD_STORAGE_MAX_FILES,
        .allocation_unit_size = CONFIG_SD_STORAGE_ALLOC_UNIT_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
#if CONFIG_SD_STORAGE_DISK_STATUS_CHECK
    /* 拔卡后 I/O 立刻返回错误，而不是一直等到超时（diskio_sdmmc 实现了 status 回调） */
    mount_cfg.disk_status_check_enable = true;
#endif

    ESP_LOGI(TAG, "mounting SD: slot=%d width=%d max_freq=%d kHz pins CLK=%d CMD=%d D0..D3=%d/%d/%d/%d "
                  "pullup=%d -> %s",
             CONFIG_SD_STORAGE_SDMMC_SLOT, CONFIG_SD_STORAGE_BUS_WIDTH, CONFIG_SD_STORAGE_MAX_FREQ_KHZ,
             CONFIG_SD_STORAGE_PIN_CLK, CONFIG_SD_STORAGE_PIN_CMD,
             CONFIG_SD_STORAGE_PIN_D0, CONFIG_SD_STORAGE_PIN_D1, CONFIG_SD_STORAGE_PIN_D2,
             CONFIG_SD_STORAGE_PIN_D3, (int)((slot.flags & SDMMC_SLOT_FLAG_INTERNAL_PULLUP) ? 1 : 0),
             CONFIG_SD_STORAGE_MOUNT_POINT);

    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_sdmmc_mount(CONFIG_SD_STORAGE_MOUNT_POINT, &host, &slot,
                                           &mount_cfg, &s_card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted an existing FATFS at '%s' (%" PRId64 " ms)",
                 CONFIG_SD_STORAGE_MOUNT_POINT, (esp_timer_get_time() - t0) / 1000);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "mount '%s' failed: %s (0x%x) after %" PRId64 " ms",
             CONFIG_SD_STORAGE_MOUNT_POINT, esp_err_to_name(err), (unsigned)err,
             (esp_timer_get_time() - t0) / 1000);

    if (err != ESP_FAIL) {
        // 卡级错误（不在/无响应/引脚不对/供电没起来）：格式化没有任何意义。
        ESP_LOGE(TAG, "  → 这不是'没有文件系统'，而是卡本身读不到（超时/无响应）。"
                      "依次检查：LDO 供电(Kconfig LDO)、引脚、卡是否插好。");
        return err;
    }

#if CONFIG_SD_STORAGE_FORMAT_IF_MOUNT_FAILED
    ESP_LOGW(TAG, "card has no usable FATFS -> formatting it "
                  "(CONFIG_SD_STORAGE_FORMAT_IF_MOUNT_FAILED=y)");
    mount_cfg.format_if_mount_failed = true;   // 这一步才会真的 mkfs
    const int64_t t1 = esp_timer_get_time();
    err = esp_vfs_fat_sdmmc_mount(CONFIG_SD_STORAGE_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount-after-format failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGW(TAG, "card was FORMATTED during this boot, then mounted at '%s' (%" PRId64 " ms)",
             CONFIG_SD_STORAGE_MOUNT_POINT, (esp_timer_get_time() - t1) / 1000);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "CONFIG_SD_STORAGE_FORMAT_IF_MOUNT_FAILED=n -> not formatting; "
                  "the volume stays unavailable");
    return err;
#endif
}

esp_err_t sd_storage_init(void)
{
    if (s_mounted) {
        ESP_LOGD(TAG, "already mounted at '%s'", CONFIG_SD_STORAGE_MOUNT_POINT);
        return ESP_OK;
    }

    esp_err_t err = sd_storage_power_on();
    if (err != ESP_OK) {
        // 不 abort：USB/WebUSB 那一半必须还能起来（探测期结论）。
        ESP_LOGE(TAG, "SD power-on failed; continuing WITHOUT the storage volume");
        return err;
    }

    err = sd_storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD volume NOT available at '%s' (%s) - continuing anyway; "
                      "MTP will enumerate without a usable storage",
                 CONFIG_SD_STORAGE_MOUNT_POINT, esp_err_to_name(err));
        return err;
    }

    s_mounted = true;

    sdmmc_card_print_info(stdout, s_card);

    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(CONFIG_SD_STORAGE_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "volume '%s': total=%" PRIu64 " B (%" PRIu64 " MiB), free=%" PRIu64 " B (%" PRIu64 " MiB)",
                 CONFIG_SD_STORAGE_MOUNT_POINT, total, total / (1024 * 1024),
                 free_bytes, free_bytes / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "esp_vfs_fat_info('%s') failed", CONFIG_SD_STORAGE_MOUNT_POINT);
    }

    int real_khz = 0;
    if (sdmmc_host_get_real_freq(CONFIG_SD_STORAGE_SDMMC_SLOT, &real_khz) == ESP_OK) {
        ESP_LOGI(TAG, "SDMMC real clock: %d kHz (requested %d kHz)",
                 real_khz, CONFIG_SD_STORAGE_MAX_FREQ_KHZ);
    }

    sd_storage_write_info_file();
    return ESP_OK;
}

esp_err_t sd_storage_deinit(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(CONFIG_SD_STORAGE_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdcard_unmount('%s') failed: %s (0x%x)",
                 CONFIG_SD_STORAGE_MOUNT_POINT, esp_err_to_name(err), (unsigned)err);
        return err;
    }
    s_card = NULL;
    s_mounted = false;

#if CONFIG_SD_STORAGE_LDO_ENABLE
    if (s_ldo != NULL) {
        esp_err_t lerr = esp_ldo_release_channel(s_ldo);
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "esp_ldo_release_channel failed: %s (0x%x)", esp_err_to_name(lerr), (unsigned)lerr);
        }
        s_ldo = NULL;
    }
#endif

    ESP_LOGI(TAG, "SD volume unmounted from '%s'", CONFIG_SD_STORAGE_MOUNT_POINT);
    return ESP_OK;
}

bool sd_storage_is_mounted(void)
{
    return s_mounted;
}

const char *sd_storage_mount_point(void)
{
    // 唯一来源：mtp_backend 的 base_path 就是这个值（改 Kconfig 一处，两侧一起变）。
    return CONFIG_SD_STORAGE_MOUNT_POINT;
}

esp_err_t sd_storage_get_capacity(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *total_bytes = 0;
    *free_bytes = 0;

    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(CONFIG_SD_STORAGE_MOUNT_POINT, total_bytes, free_bytes);
}

esp_err_t sd_storage_get_info(sd_storage_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    // 这些值在未挂载时也有意义（"存储当前是什么状态"要能一行打出来）。
    out->mounted = s_mounted;
    out->mount_point = CONFIG_SD_STORAGE_MOUNT_POINT;
    out->max_freq_khz = CONFIG_SD_STORAGE_MAX_FREQ_KHZ;
#if CONFIG_SD_STORAGE_LDO_ENABLE
    out->ldo_used = (s_ldo != NULL);
#endif
    out->ldo_channel = CONFIG_SD_STORAGE_LDO_CHANNEL;
    out->ldo_voltage_mv = CONFIG_SD_STORAGE_LDO_VOLTAGE_MV;

    if (!s_mounted) {
        return ESP_OK;
    }

    (void)esp_vfs_fat_info(CONFIG_SD_STORAGE_MOUNT_POINT, &out->total_bytes, &out->free_bytes);
    (void)sdmmc_host_get_real_freq(CONFIG_SD_STORAGE_SDMMC_SLOT, &out->real_freq_khz);

    if (s_card != NULL) {
        memcpy(out->card_name, s_card->cid.name, sizeof(s_card->cid.name));
        out->card_name[sizeof(s_card->cid.name)] = '\0';
    }
    return ESP_OK;
}
