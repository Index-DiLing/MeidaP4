// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I2 契约 —— **帧源**（视频管线 → WebUSB 编码器）
//
//   生产者（本文件）：Lead            实现：components/video_pipe/（WS-video）
//   消费者：components/usb_vendor/（WS-webusb）—— 它只通过本接口取帧，不关心怎么编出来的
//
// 设计原则（plan §批3）：**分两阶段，先 MJPEG 打通再上 H.264**。
//   阶段一 MJPEG：LVGL 离屏渲染(RGB565) → PPA → 硬件 JPEG → 一帧一帧吐出来
//   阶段二 H.264：同一接口，只换 fmt 与编码器
//   因此本接口**必须**同时容纳"整帧"与"编码后的字节流"，且**不暴露任何编码器细节**。
//
// ⚠️ 帧缓冲生命周期：`get_frame()` 借出的内存在 `release_frame()` 之前归调用方独占；
//    **不允许**持有超过一次 `get_frame()` 的间隔（管线需要回收缓冲）。超时未释放由实现兜底回收。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 帧格式（与线协议 VIDEO 消息头的 fmt 字段一一对应，见 docs/protocol.md §5.2） */
typedef enum {
    WU_FMT_MJPEG = 0,
    WU_FMT_RGB565 = 1,
    WU_FMT_H264 = 2,
} wu_frame_fmt_t;

/** 一帧编码输出 */
typedef struct {
    const uint8_t *data;  /**< 编码后的字节（MJPEG/H264 为码流；RGB565 为原始像素） */
    size_t len;
    uint32_t frame_seq;   /**< 编码帧序号，逐帧 +1（丢帧时跳号） */
    uint32_t ts_us;       /**< 采集时刻（us，低 32 位） */
    wu_frame_fmt_t fmt;
    uint16_t width;
    uint16_t height;
    bool keyframe;        /**< H.264 用；MJPEG 恒 true */
} video_frame_t;

/* ============================ 配置 ============================ */

typedef struct {
    /** 渲染分辨率（LVGL 离屏缓冲尺寸） */
    uint16_t width;
    uint16_t height;
    /** 目标帧率（上下限由实现内部限幅） */
    uint8_t fps;
    /** 起始格式：阶段一填 WU_FMT_MJPEG */
    wu_frame_fmt_t fmt;
    /** 编码质量 1..100（仅 MJPEG/H264 有效）；0 = 用默认 */
    uint8_t quality;
    /**
     * 是否在帧上叠加时间戳/帧号水印。默认 true —— 用于**真机验收**时肉眼确认
     * "没有冻结、没有乱序"（配合网页侧画布显示帧号）。
     */
    bool overlay_stats;
    /** 帧缓冲数量（双缓冲=2，三缓冲=3）。0 = 默认 3 */
    uint8_t buffer_count;
} video_pipe_config_t;

/** 取默认配置：**1280x720 @30 MJPEG q=80**（plan §7 已定档：720p30 起步，1080p30 为目标） */
void video_pipe_config_default(video_pipe_config_t *out);

/* ============================ 生命周期 ============================ */

/**
 * @brief 建管线（分配 PSRAM 帧缓冲、初始化 LVGL 离屏显示、起渲染任务）。
 *
 * 必须在 `usb_composite_start()` 之前调用（编码器需要先把缓冲准备好）。
 * 幂等：重复调用返回 ESP_OK。
 */
esp_err_t video_pipe_start(const video_pipe_config_t *cfg);

/** 停管线并释放资源。 */
esp_err_t video_pipe_stop(void);

/**
 * @brief 运行时切换编码格式（阶段一 MJPEG ↔ 阶段二 H.264）。
 *
 * 允许在管线运行中调用；实现负责在**帧边界**切换，不会吐出一半 MJPEG 一半 H.264 的流。
 * 若目标格式不被编译进固件（例如 `CONFIG_VIDEO_PIPE_ENABLE_H264=n`）返回 `ESP_ERR_NOT_SUPPORTED`。
 */
esp_err_t video_pipe_set_format(wu_frame_fmt_t fmt);

/** 运行时改质量（1..100） */
esp_err_t video_pipe_set_quality(uint8_t quality);

/* ============================ 帧源接口（I2 核心，冻结） ============================ */

/**
 * @brief 取下一帧（生产者的主入口）。
 *
 * @param out        输出（必需）
 * @param timeout_ms 0 = 非阻塞；> 0 = 最多等这么久
 * @return
 *   - ESP_OK            拿到一帧，调用方**必须**随后 `release_frame()`
 *   - ESP_ERR_TIMEOUT   超时内没有新帧（正常情况，调用方继续循环）
 *   - ESP_ERR_INVALID_STATE 管线没启动
 *   - ESP_FAIL          管线内部错误（编码器故障等）；调用方不应忙等
 */
esp_err_t video_pipe_get_frame(video_frame_t *out, uint32_t timeout_ms);

/** 归还帧缓冲。`frame` 为 NULL 时安全返回。 */
void video_pipe_release_frame(video_frame_t *frame);

/** 是否有可立刻取到的帧（供 WebUSB 侧做"有帧才发"的调度，不做阻塞） */
bool video_pipe_frame_ready(void);

/**
 * @brief 丢帧通知（**背压**）。
 *
 * WebUSB 侧发现链路发不出去（EP3 忙 / 主机没在收）时调用，
 * 让管线**主动降低**产出（跳帧 / 降质量），而不是把 PSRAM 帧缓冲耗尽。
 *
 * @param dropped 本次被丢掉的帧数（≥1）
 */
void video_pipe_report_drop(uint32_t dropped);

/* ============================ UI 命令（来自网页） ============================ */

/**
 * @brief 把网页发来的 UI 命令转给 LVGL（线协议 CTRL `WU_OP_UI_CMD`）。
 *
 * 命令体是 UTF-8 的 `key=value` 串（例如 `theme=dark`、`page=net`、`text=Hello`），
 * 具体键值由 WS-video 在 `components/ui/` 里定义；本接口只负责"投递到 UI 任务"。
 */
esp_err_t video_pipe_post_ui_cmd(const char *utf8, size_t len);

/* ============================ 统计（必须回填到 TELEM 通道） ============================ */

typedef struct {
    uint32_t frames_out;      /**< 成功产出的完整帧数 */
    uint32_t frames_dropped;  /**< 因为背压/缓冲不足被丢的帧数 */
    uint32_t encode_us_avg;   /**< 单帧编码耗时（us，指数滑动平均） */
    uint32_t encode_us_max;
    uint32_t render_us_avg;   /**< LVGL 渲染耗时（us，指数滑动平均） */
    uint32_t bytes_last;      /**< 上一帧字节数 */
    uint32_t bytes_avg;       /**< 平均帧字节数（滑动平均） */
    uint32_t kbps;            /**< 最近 1 s 实测码率 */
    uint32_t fps_actual_x100; /**< 最近 1 s 实测帧率 ×100 */
} video_pipe_stats_t;

void video_pipe_get_stats(video_pipe_stats_t *out);

#ifdef __cplusplus
}
#endif
