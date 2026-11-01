// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · UI（LVGL 9.2 离屏显示 + 仪表盘）
//
// 本组件只做两件事，且**不依赖** `video_pipe`（避免循环依赖）：
//   1) 用 LVGL 9.2 在 PSRAM 里做一块 1280x720 RGB565 的**离屏显示**（DIRECT 模式、
//      双缓冲、64B 对齐），没有面板、没有触摸：flush_cb 只记录"最近一次完整刷新的
//      帧缓冲指针"，然后立刻 `lv_display_flush_ready()`。
//   2) 仪表盘页面（标题 / 运行时长 / 墙钟 / SD 剩余空间 / fps+编码统计 / 动态色条 /
//      帧序号）+ 网页 UI 命令（`key=value`）投递。
//
// 帧缓冲生命周期（与 video_pipe 的分工，重要）：
//   video_pipe 的渲染任务每周期调用一次 `ui_render_once()`，然后立刻用
//   `ui_get_frame()` 取**同一块**帧缓冲喂硬件 JPEG。DIRECT 模式下 LVGL 在这之后
//   会渲染到**另一块**缓冲（双缓冲交换只发生在最后一次 flush 之后），所以编码期间
//   这块内存不会被 LVGL 改写 —— 这就是"零拷贝"能成立的全部理由。
//
// 写入者：WS-V（video_pipe / ui）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 仪表盘要显示的数值快照（由 video_pipe 每帧填一次） */
typedef struct {
    uint32_t seq;             /**< 最新编码帧序号 */
    uint32_t frames_out;      /**< 累计产出帧数 */
    uint32_t frames_dropped;  /**< 累计丢帧数 */
    uint32_t fps_x100;        /**< 实测 fps ×100（1 s 窗口） */
    uint32_t fps_target;      /**< 当前目标 fps（含自适应降档） */
    uint32_t encode_us_avg;
    uint32_t encode_us_max;
    uint32_t render_us_avg;
    uint32_t kbps;            /**< 最近 1 s 码率 */
    uint32_t bytes_last;
    uint32_t bytes_avg;
    uint32_t psram_free;      /**< PSRAM 空闲字节（真机核对用） */
    uint8_t  quality;         /**< 当前生效的 JPEG 质量 */
    uint8_t  buffers;         /**< 帧缓冲数量 */
} ui_stats_t;

/**
 * @brief 初始化 LVGL + 离屏显示 + 仪表盘。
 *
 * 分配两块 `width*height*2` 字节的 PSRAM 帧缓冲（64B 对齐）。失败返回错误但不 abort。
 */
esp_err_t ui_init(uint16_t width, uint16_t height);

/** 反初始化（释放帧缓冲、lv_deinit）。 */
void ui_deinit(void);

/** UI 是否已就绪 */
bool ui_is_ready(void);

/**
 * @brief 跑一轮 LVGL（先按最新快照刷新文字/动态元素，再 `lv_timer_handler()`）。
 * @return 本轮渲染耗时（us）
 */
uint32_t ui_render_once(void);

/**
 * @brief 取最近一次完整刷新后的 RGB565 帧缓冲（整帧，stride = width*2）。
 * @param rgb565 输出：帧缓冲首地址（PSRAM，64B 对齐）
 * @param bytes  输出：缓冲字节数
 * @return 还没有任何一帧渲染完成时返回 false
 */
bool ui_get_frame(const uint8_t **rgb565, uint32_t *bytes);

/** 帧缓冲宽高 */
void ui_get_geometry(uint16_t *width, uint16_t *height);

/**
 * @brief 更新仪表盘数值（只存快照，真正的 lv_label_set_text 在 ui_render_once 里做）。
 *        可从渲染任务以外的任务调用。
 */
void ui_update_stats(const ui_stats_t *snap);

/**
 * @brief 投递网页 UI 命令（`key=value`，多个用 `;`/`&`/`,` 分隔）。
 *
 * 支持：`title=<text>`、`theme=dark|light`、`page=main|stats`、`text=<text>`、
 *       `overlay=0|1`。未知键只打 WARN，不影响其他键。
 */
esp_err_t ui_post_cmd(const char *utf8, size_t len);

/** 消费命令队列（**必须在渲染任务里**调用，因为它会直接操作 lv_obj） */
void ui_apply_cmds(void);

#ifdef __cplusplus
}
#endif
