// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 「PC（MTP）↔ P4（FILE）文件级共享」探针
//
// 为什么要它：MTP 与 WebUSB FILE 通道都落在**同一个 FATFS 卷**（/sdcard）上。光"两个功能
// 各自能读写"不能证明它们是共享的（各自写到别处也能通过）。必须有一条**跨侧可见的证据链**：
//
//   ① 心跳：P4 每 5 s 重写 /sdcard/MeidaP4-P4-HEARTBEAT.txt（seq 递增）
//        → 在 **PC 上**用资源管理器（MTP）打开它，seq 在涨 ⇒ PC 看到的是 P4 写的同一份文件。
//   ② 监听：PC 写 /sdcard/PC2P4.TXT，P4 每 2 s 检查一次，长度/mtime/内容指纹变化就
//      在串口日志里打出前 128 字节
//        → 串口出现 "P4 看到 PC 写入" ⇒ P4 看到的是 PC 刚写的那份文件。
//   ② 是**关键证据**：它排除了"MTP 和 FILE 各自维护一份缓存/副本"的可能。
//
// 线程/并发：
//   * 探针用**自己的 FILE\***，不与 FILE 服务的句柄表共享任何 FILE*；
//   * 与 MTP（esp_tinyusb 任务）并发访问同一卷由 VFS 的卷级互斥保证
//     （`CONFIG_FATFS_LINK_LOCK=y` → vfs_fat.c 的 fat_ctx->lock）；
//   * 统计写入走 fs_stats_begin()/fs_stats_end()（内部头的唯一实例）。
//
// 失败策略：SD 未挂载时什么都不做（静默等下一次），绝不 abort、绝不阻塞。

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_storage.h"

#include "file_service.h"
#include "file_service_internal.h"

static const char *TAG = "file_probe";

/* ============================ 常量 ======================================== */

/** 探针轮询节拍（两个周期都是它的整数倍，见下） */
#define PROBE_TICK_MS        1000
/** 心跳周期：5 s */
#define PROBE_HB_TICKS       5
/** PC2P4 轮询周期：2 s */
#define PROBE_POLL_TICKS     2
/**
 * SD 容量刷新周期（按心跳次数算）：12 × 5 s = 60 s。
 *
 * 为什么不每 5 s 查一次：`esp_vfs_fat_info()` 内部是 FatFs 的 `f_getfree()`。FAT32 上
 * 如果卷的 FSInfo 不可信（格式化工具不同 / 有脏标记），FatFs 会**扫描整张 FAT**才能
 * 算出空闲簇 —— 32 GB / 4 KB 簇 ⇒ 8M 簇 ⇒ ~32 MB 读，而这段扫描是在 VFS 的**卷级锁**
 * 里跑的，会把 MTP（PC 侧）一起堵住。缓存 + 每 60 s 刷新把最坏情况从"每 5 s"降到"每 60 s"。
 */
#define PROBE_SD_INFO_TICKS  12
/** 探测任务 */
#define PROBE_TASK_STACK     4096
#define PROBE_TASK_PRIO      3
/** 路径缓冲（根 + 文件名） */
#define PROBE_PATH_MAX       96
/** 日志里最多打 PC2P4.TXT 的前多少个字节 */
#define PROBE_PEEK_BYTES     128

/** 心跳文件（P4 → PC 的"我还在跑"证据） */
#define PROBE_HB_NAME        "MeidaP4-P4-HEARTBEAT.txt"
/** PC → P4 的投递文件（PC 写入，P4 每 2 s 检查） */
#define PROBE_PC2P4_NAME     "PC2P4.TXT"
/** 共享说明（只在不存在时创建，不覆盖 PC 编辑过的内容） */
#define PROBE_README_NAME    "README-SHARE.txt"

/* ============================ 状态 ======================================== */

static bool s_probe_started;

/* 以下状态**只被探针任务访问**，不需要锁 */
static bool     s_readme_done;      /**< 本次挂载内是否已检查/创建说明文件 */
static bool     s_pc2p4_seen;       /**< 上次轮询时 PC2P4.TXT 是否存在 */
static uint32_t s_pc2p4_len;        /**< 上次看到的长度 */
static uint32_t s_pc2p4_mtime;      /**< 上次看到的 mtime（FAT 2 s 分辨率，只作辅助） */
static uint32_t s_pc2p4_fnv;        /**< 上次看到的内容指纹（前 128 B 的 FNV-1a） */

/* SD 容量缓存（见 PROBE_SD_INFO_TICKS 的注释：避免每 5 s 触发一次潜在的全 FAT 扫描） */
static uint64_t s_sd_total;         /**< 卷总容量（字节） */
static uint64_t s_sd_free;          /**< 卷可用（字节） */
static bool     s_sd_info_valid;    /**< 缓存是否有效（每次挂载后作废） */
static uint32_t s_sd_info_age;      /**< 距上次刷新过了几个心跳 */

/* ============================ 小工具 ====================================== */

/** 拼一个根内的探针文件路径；放不下返回 false（绝不截断 —— 截断会指到别的文件） */
static bool probe_path(char *out, size_t cap, const char *name)
{
    const char *root = sd_storage_mount_point();
    size_t rlen;
    size_t nlen;

    if (root == NULL || name == NULL) {
        return false;
    }
    rlen = strlen(root);
    nlen = strlen(name);
    if (rlen + 1 + nlen + 1 > cap) {
        return false;
    }
    memcpy(out, root, rlen);
    out[rlen] = '/';
    memcpy(out + rlen + 1, name, nlen + 1);
    return true;
}

/** FNV-1a 32 位（只为"内容变了没"，不做安全用途） */
static uint32_t probe_fnv1a(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

/* ============================ 心跳文件 ==================================== */

/**
 * @brief 重写 /sdcard/MeidaP4-P4-HEARTBEAT.txt（P4 → PC 的可见性证据）。
 *
 * 内容全部 ASCII，便于 PC 上用记事本直接看；每 5 s 覆盖一次，`seq` 必须单调递增。
 */
static void probe_write_heartbeat(uint32_t seq)
{
    char path[PROBE_PATH_MAX];
    char body[480];
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    int n;
    FILE *f;
    size_t wrote;

    if (!probe_path(path, sizeof(path), PROBE_HB_NAME)) {
        return;
    }

    /* 卷容量：带缓存（原因见 PROBE_SD_INFO_TICKS）；拿不到就写上一份/0，不影响心跳本身 */
    if (!s_sd_info_valid || s_sd_info_age >= PROBE_SD_INFO_TICKS) {
        if (esp_vfs_fat_info(sd_storage_mount_point(), &s_sd_total, &s_sd_free) == ESP_OK) {
            s_sd_info_valid = true;
            s_sd_info_age = 0;
        }
    }
    s_sd_info_age++;
    total = s_sd_total;
    free_bytes = s_sd_free;

    n = snprintf(body, sizeof(body),
                 "MeidaP4 P4 HEARTBEAT (written by ESP32-P4)\r\n"
                 "file=%s\r\n"
                 "seq=%" PRIu32 "\r\n"
                 "uptime_ms=%" PRIu64 "\r\n"
                 "heap_free=%" PRIu32 "\r\n"
                 "psram_free=%" PRIu32 "\r\n"
                 "sd_total_kb=%" PRIu64 "\r\n"
                 "sd_free_kb=%" PRIu64 "\r\n"
                 "writer=ESP32-P4 /sdcard (FATFS)\r\n"
                 "note=seq increases every 5 s; if a PC sees it growing over MTP, "
                 "the PC and the P4 share the very same file.\r\n",
                 PROBE_HB_NAME, seq,
                 (uint64_t)(esp_timer_get_time() / 1000),
                 (uint32_t)esp_get_free_heap_size(),
                 (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 total / 1024u, free_bytes / 1024u);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(body)) {
        n = (int)sizeof(body) - 1;
    }

    f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "心跳写打不开 '%s': errno=%d（下一轮再试）", path, errno);
        return;
    }
    wrote = fwrite(body, 1, (size_t)n, f);
    if (fclose(f) != 0) {
        ESP_LOGW(TAG, "心跳写关闭失败 '%s': errno=%d", path, errno);
    }
    if (wrote != (size_t)n) {
        ESP_LOGW(TAG, "心跳写不完整 '%s': %u/%u B", path, (unsigned)wrote, (unsigned)n);
    }

    fs_stats_begin();
    fs_shared_stats.share_hb_writes++;
    fs_stats_end();
}

/* ============================ PC2P4 监听 ================================== */

/**
 * @brief 检查 /sdcard/PC2P4.TXT（PC 写入）—— 变化时把前 128 字节打进日志。
 *
 * 变化判据：存在性 / 长度 / mtime / **前 128 B 的 FNV 指纹**任一变化。
 * 加指纹是因为 FAT 的 mtime 只有 2 s 分辨率：PC 在 2 s 内重写同样长度的内容是可能发生的，
 * 只看 size+mtime 会漏报 —— 而"漏报"恰好会让这条证据链在最需要它的时候失效。
 *
 * @param force true = 忽略"与上次相同"的短路（挂载后第一次检查用）
 */
static void probe_poll_pc2p4(bool force)
{
    char path[PROBE_PATH_MAX];
    struct stat sb;
    uint8_t head[PROBE_PEEK_BYTES];
    char text[PROBE_PEEK_BYTES + 1];
    size_t n = 0;
    uint32_t fnv;
    FILE *f;
    int i;
    bool changed;

    if (!probe_path(path, sizeof(path), PROBE_PC2P4_NAME)) {
        return;
    }

    if (stat(path, &sb) != 0) {
        if (s_pc2p4_seen) {
            /* 文件被删掉：只在状态切换时说一句，不存在时**静默**（任务书要求） */
            s_pc2p4_seen = false;
            s_pc2p4_len = 0;
            s_pc2p4_mtime = 0;
            s_pc2p4_fnv = 0;
            ESP_LOGD(TAG, "PC2P4.TXT 已不存在（等待 PC 写入）");
        }
        return;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        return;     /* 争用/瞬态失败：下一轮再看，不刷日志 */
    }
    n = fread(head, 1, sizeof(head), f);
    (void)fclose(f);
    fnv = probe_fnv1a(head, n);

    changed = (force || !s_pc2p4_seen)
              || ((uint32_t)sb.st_size != s_pc2p4_len)
              || ((uint32_t)sb.st_mtime != s_pc2p4_mtime)
              || (fnv != s_pc2p4_fnv);
    if (!changed) {
        return;
    }

    for (i = 0; i < (int)n; i++) {
        text[i] = (head[i] >= 0x20 && head[i] < 0x7F) ? (char)head[i] : '.';
    }
    text[n] = '\0';

    ESP_LOGI(TAG, "P4 看到 PC 写入: %s size=%ld mtime=%" PRIu32 " first=%uB \"%s\"",
             path, (long)sb.st_size, (uint32_t)sb.st_mtime, (unsigned)n, text);

    s_pc2p4_seen = true;
    s_pc2p4_len = (uint32_t)sb.st_size;
    s_pc2p4_mtime = (uint32_t)sb.st_mtime;
    s_pc2p4_fnv = fnv;

    fs_stats_begin();
    fs_shared_stats.share_probe_hits++;
    fs_shared_stats.share_pc2p4_len = (uint32_t)sb.st_size;
    fs_stats_end();
}

/* ============================ 共享说明 ==================================== */

/** 首次挂载时创建说明文件（已存在就不碰 —— 可能是 PC 编辑过的） */
static void probe_write_readme(void)
{
    char path[PROBE_PATH_MAX];
    struct stat sb;
    FILE *f;
    static const char *text =
        "MeidaP4 —— TF 卡共享目录（PC 与 ESP32-P4 共用同一个 FATFS 卷）\r\n"
        "\r\n"
        "这是 ESP32-P4 固件（MeidaP4）SD 卡的根目录：\r\n"
        "  * PC 侧：通过 MTP（资源管理器里的便携设备）读写；\r\n"
        "  * P4 侧：通过 WebUSB FILE 通道（浏览器文件管理器）读写；\r\n"
        "  * 两边是同一个卷、同一份文件，不是两份拷贝。\r\n"
        "\r\n"
        "固件自动维护的自检文件（可读、可删，删了会重建）：\r\n"
        "  * MeidaP4-P4-HEARTBEAT.txt  P4 每 5 秒重写（seq 递增 = P4 在跑）\r\n"
        "  * PC2P4.TXT                 PC 写这个文件，P4 每 2 秒检查并在串口日志打出前 128 字节\r\n"
        "\r\n"
        "验收步骤：\r\n"
        "  1) PC 上用记事本写 PC2P4.TXT 并保存；\r\n"
        "  2) 串口日志应出现 \"P4 看到 PC 写入: ...\"（证明 P4 立刻看到 PC 的写入）；\r\n"
        "  3) 从 PC（MTP）打开 MeidaP4-P4-HEARTBEAT.txt，seq 应该在增长；\r\n"
        "  4) 用浏览器文件管理器（WebUSB FILE 通道）写一个文件，PC 侧刷新应看到它。\r\n"
        "\r\n"
        "实现：components/file_service（WS-F）\r\n";

    if (s_readme_done) {
        return;
    }
    if (!probe_path(path, sizeof(path), PROBE_README_NAME)) {
        s_readme_done = true;
        return;
    }
    if (stat(path, &sb) == 0) {
        s_readme_done = true;       /* 已存在：保持 PC 的版本 */
        return;
    }
    f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "共享说明创建失败 '%s': errno=%d", path, errno);
        return;                     /* 下一轮再试 */
    }
    (void)fputs(text, f);
    (void)fclose(f);
    s_readme_done = true;
    ESP_LOGI(TAG, "共享说明已创建: %s", path);
}

/* ============================ 任务 ======================================== */

static void probe_task(void *arg)
{
    uint32_t tick = 0;
    uint32_t hb_seq = 0;
    uint32_t last_hb_tick = 0;
    uint32_t last_poll_tick = 0;
    bool mounted_prev = false;

    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_TICK_MS));
        tick++;

        if (!sd_storage_is_mounted()) {
            if (mounted_prev) {
                ESP_LOGW(TAG, "SD 卷已消失（%s）—— 探针挂起，等待重新挂载",
                         sd_storage_mount_point());
                mounted_prev = false;
                s_readme_done = false;
                s_pc2p4_seen = false;
                s_sd_info_valid = false;    /* 重新挂载后容量必须重算 */
            }
            continue;
        }

        if (!mounted_prev) {
            /* 挂载后第一次：立刻写一版心跳 + 立刻查一次 PC2P4（"开机就能看到"） */
            mounted_prev = true;
            s_sd_info_valid = false;
            ESP_LOGI(TAG, "SD 已挂载（%s）—— 共享探针开始工作", sd_storage_mount_point());
            probe_write_readme();
            hb_seq++;
            probe_write_heartbeat(hb_seq);
            last_hb_tick = tick;
            probe_poll_pc2p4(true);
            last_poll_tick = tick;
            continue;
        }

        if ((tick - last_hb_tick) >= PROBE_HB_TICKS) {
            last_hb_tick = tick;
            hb_seq++;
            probe_write_heartbeat(hb_seq);
        }
        if ((tick - last_poll_tick) >= PROBE_POLL_TICKS) {
            last_poll_tick = tick;
            probe_poll_pc2p4(false);
        }
    }
}

/* ============================ 生命周期 ==================================== */

esp_err_t file_share_probe_start(void)
{
    const char *root = sd_storage_mount_point();

    if (s_probe_started) {
        return ESP_OK;
    }
    if (xTaskCreate(probe_task, "fs_probe", PROBE_TASK_STACK, NULL, PROBE_TASK_PRIO, NULL)
            != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_probe_started = true;

    ESP_LOGI(TAG, "共享探针已启动: 心跳 %s/%s（每 %d s，seq 递增）",
             (root == NULL) ? "?" : root, PROBE_HB_NAME, PROBE_HB_TICKS * PROBE_TICK_MS / 1000);
    ESP_LOGI(TAG, "                  监听 %s/%s（每 %d s，变化时打前 %d B）",
             (root == NULL) ? "?" : root, PROBE_PC2P4_NAME,
             PROBE_POLL_TICKS * PROBE_TICK_MS / 1000, PROBE_PEEK_BYTES);
    ESP_LOGI(TAG, "                  说明 %s/%s（仅首次挂载且不存在时创建）",
             (root == NULL) ? "?" : root, PROBE_README_NAME);
    ESP_LOGI(TAG, "                  任务 fs_probe prio=%d stack=%d | 与 MTP 共享同一卷，"
                  "靠 FATFS 卷级互斥（CONFIG_FATFS_LINK_LOCK）串行化",
             PROBE_TASK_PRIO, PROBE_TASK_STACK);
    return ESP_OK;
}

bool file_share_probe_is_started(void)
{
    return s_probe_started;
}
