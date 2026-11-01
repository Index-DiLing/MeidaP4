// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · file_service 组件内部头（**不对外**；只在 components/file_service/*.c 之间用）
//
// 为什么需要它：FILE 服务任务（file_service.c）与共享探针任务（file_share_probe.c）
// 都往同一个统计结构里写计数，而 main 的 10 s 健康遥测会从第三个任务读它 —— 所以统计
// 必须有一个**唯一的实例 + 唯一的锁**。把它放在一个内部头里，两个 .c 就不会各自造一份。
#pragma once

#include "esp_err.h"

#include "file_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 统计实例（**唯一**）。读写必须成对使用 fs_stats_begin()/fs_stats_end()。 */
extern file_service_stats_t fs_shared_stats;

/** 进入/退出统计临界区（自旋锁；只包几条赋值，绝不包 I/O） */
void fs_stats_begin(void);
void fs_stats_end(void);

/**
 * @brief 启动"PC ↔ P4 文件级共享"探针任务（幂等）。
 *
 * 心跳文件每 5 s 重写；PC2P4.TXT 每 2 s 轮询；README-SHARE.txt 首次挂载时创建。
 * 由 file_service_start() 调用；失败只打日志，不影响 FILE 通道。
 */
esp_err_t file_share_probe_start(void);

/** 探针任务是否已启动 */
bool file_share_probe_is_started(void);

#ifdef __cplusplus
}
#endif
