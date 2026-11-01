// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I2 帧源（视频管线）—— 真实实现
//
// 数据流（每帧一个周期，全部在同一个任务里，**没有任何跨任务的 LVGL 调用**）：
//
//   lv_timer_handler()  →  ui_get_frame()  →  jpeg_encoder_process()  →  帧缓冲池  →  I2
//   （LVGL 离屏 DIRECT 双缓冲，flush 后即是完整 1280x720 RGB565）          3 个 slot     get_frame/release_frame
//
// 关键事实（改代码前请先读）：
//  1) **零拷贝**：DIRECT 模式下 LVGL 只重画脏区，并在每次刷新前把另一块缓冲同步好，
//     所以"最后一次 flush 的那块缓冲"一定是一张完整图像；而且**交换发生在最后一次
//     flush 之后**，编码期间 LVGL 只会写另一块 → 直接把这块 PSRAM 喂给硬件 JPEG 是安全的。
//  2) **对齐/内存**：JPEG 的输入必须是"非加密 PSRAM + 16B 对齐"（驱动内部会做 cache
//     writeback），输出必须是 cache line 对齐 —— 所以输出缓冲一律走 `jpeg_alloc_encoder_mem`，
//     输入直接用 ui 里 64B 对齐的 PSRAM 帧缓冲。
//  3) **背压**：`video_pipe_report_drop()` 记"跳帧债"，渲染任务按债务主动跳帧；持续背压还会
//     自适应降 fps / 降 quality（背压消失 5 s 后逐档恢复）。缓冲池永远有上界，不会耗尽 PSRAM。
//  4) **无消费端**：如果 >1.5 s 没人 `get_frame()`（主机没在收），只刷 UI 不编码 —— 否则 3 个
//     缓冲会瞬间被"没人要的帧"占满，白白烧 CPU/PSRAM。
//
// 统计（全部真实回填）：frames_out / frames_dropped / encode_us_avg(EWMA) / encode_us_max /
// render_us_avg(EWMA) / bytes_last / bytes_avg(EWMA) / kbps(1 s 窗口) / fps_actual_x100(1 s 窗口)。
//
// 写入者：WS-V。I2 契约（include/video_pipe.h）**冻结**，签名不得改。

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/jpeg_encode.h"

#include "video_pipe.h"
#include "ui.h"

static const char *TAG = "video_pipe";

/* ============================ 可调参数 ============================ */

#define VP_MAX_BUFS             4       /* slot 上限（契约 buffer_count 是 uint8，这里限到 4） */
#define VP_MIN_BUFS             2
#define VP_DEFAULT_BUFS         3

#define VP_ENC_TIMEOUT_MS       500     /* jpeg_encoder_process 阻塞上限（引擎 timeout_ms） */
#define VP_SUBSAMPLE            JPEG_DOWN_SAMPLING_YUV420
#define VP_QUALITY_DEFAULT      80
#define VP_QUALITY_MIN          30      /* 自适应降质量的下限 */
#define VP_FPS_DEFAULT          30
#define VP_FPS_MIN              5
#define VP_FPS_MAX              60

#define VP_PIXEL_REVERSE        false   /* RGB565 字节序：先 false；花屏/偏色时用 ui cmd `pixrev=1` 翻 */

#define VP_TASK_STACK           12288
#define VP_TASK_PRIO            5       /* 高于 uv_video(4)：渲染/编码不能被发送任务饿死 */

#define VP_RECLAIM_US           (5 * 1000 * 1000)   /* 借出帧超时兜底回收 */
#define VP_IDLE_US              (1500 * 1000)       /* 这么久没人调 get_frame → 认为没有消费端 */
#define VP_IDLE_LOG_US          (30 * 1000 * 1000)  /* 空闲日志：状态变化打一条，之后每 30 s 最多一条 */
#define VP_DROP_DEBT_MAX        16                  /* 跳帧债上限（≈0.5 s @30fps） */
#define VP_PRESSURE_THRESH      12                  /* 累计背压到这个数就降一档 */
#define VP_RECOVER_US           (2 * 1000 * 1000)   /* 无背压 2 s 后开始逐档恢复 */

/* ============================ 类型 ============================ */

typedef enum {
    SLOT_FREE = 0,
    SLOT_ENCODING,
    SLOT_READY,
    SLOT_BORROWED,
} vp_slot_state_t;

typedef struct {
    uint8_t        *data;      /* JPEG 输出缓冲（jpeg_alloc_encoder_mem） */
    size_t          cap;
    size_t          len;
    uint32_t        seq;
    uint32_t        ts_us;
    vp_slot_state_t state;
    int64_t         borrow_us;
} vp_slot_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool        started;
    bool        stopping;
    TaskHandle_t task;

    /* 配置（生效值） */
    uint16_t    w;
    uint16_t    h;
    uint8_t     nbuf;
    uint8_t     quality;       /* 当前生效质量（可能被自适应调低） */
    uint8_t     quality_user;  /* 用户设定质量 */
    uint8_t     fps_target;
    uint8_t     fps_eff;       /* 当前目标 fps（可能被自适应调低） */
    uint32_t    jpeg_timeout_ms;
    bool        pixel_reverse;
    wu_frame_fmt_t fmt;

    size_t      out_cap;       /* 每个输出缓冲的字节上限 */
    size_t      out_alloc;     /* 实际分配到的字节数（jpeg_alloc_encoder_mem 返回值） */
    vp_slot_t   slot[VP_MAX_BUFS];
    jpeg_encoder_handle_t enc;

    QueueHandle_t free_q;      /* uint8_t idx */
    QueueHandle_t ready_q;     /* uint8_t idx */

    video_pipe_stats_t stats;
    uint32_t    frame_seq;
    uint32_t    drop_debt;
    uint32_t    pressure;
    int64_t     last_recover_us;
    int64_t     last_get_us;       /* 最近一次 get_frame() 调用（**含超时**）：判断"有没有消费端" */
    int64_t     last_idle_log_us;  /* 空闲日志限流（30 s） */
    bool        idle;              /* 当前是否处于"无消费端"状态（只驱动日志状态机） */

    /* 1 s 窗口 */
    int64_t     win_start_us;
    uint32_t    win_frames;
    uint32_t    win_bytes;

    /* 错误日志限流 */
    uint32_t    enc_fail_count;
} s;

/* ============================ 小工具 ============================ */

static uint32_t ewma(uint32_t avg, uint32_t sample)
{
    int32_t d = (int32_t)sample - (int32_t)avg;
    return (uint32_t)((int32_t)avg + d / 8);
}

static void vp_stats_add_render(uint32_t render_us)
{
    portENTER_CRITICAL(&s_mux);
    s.stats.render_us_avg = ewma(s.stats.render_us_avg, render_us);
    portEXIT_CRITICAL(&s_mux);
}

static void vp_stats_drop(uint32_t n)
{
    portENTER_CRITICAL(&s_mux);
    s.stats.frames_dropped += n;
    portEXIT_CRITICAL(&s_mux);
}

/** 1 s 窗口到期 → 结算 fps_actual_x100 / kbps（两个都是"实测"，不是目标值） */
static void vp_stats_window(int64_t now)
{
    int64_t elapsed = now - s.win_start_us;
    if (elapsed < 1000000) {
        return;
    }
    uint32_t fps_x100 = (elapsed > 0)
                        ? (uint32_t)((uint64_t)s.win_frames * 100000000ULL / (uint64_t)elapsed) : 0;
    uint32_t kbps = (elapsed > 0)
                    ? (uint32_t)((uint64_t)s.win_bytes * 8000ULL / (uint64_t)elapsed) : 0;

    portENTER_CRITICAL(&s_mux);
    s.stats.fps_actual_x100 = fps_x100;
    s.stats.kbps = kbps;
    portEXIT_CRITICAL(&s_mux);

    s.win_start_us = now;
    s.win_frames = 0;
    s.win_bytes = 0;
}

/** 借出超时的缓冲兜底回收（契约要求："超时未释放由实现兜底回收"） */
static void vp_reclaim(int64_t now)
{
    for (uint8_t i = 0; i < s.nbuf; i++) {
        bool reclaim = false;

        portENTER_CRITICAL(&s_mux);
        if (s.slot[i].state == SLOT_BORROWED && (now - s.slot[i].borrow_us) > VP_RECLAIM_US) {
            s.slot[i].state = SLOT_FREE;
            reclaim = true;
        }
        portEXIT_CRITICAL(&s_mux);

        if (reclaim) {
            uint8_t idx = i;
            ESP_LOGW(TAG, "帧缓冲 #%u 借出超过 %lld ms 未归还，兜底回收（消费者没有按契约 release）",
                     (unsigned)i, (long long)(VP_RECLAIM_US / 1000));
            vp_stats_drop(1);
            (void)xQueueSend(s.free_q, &idx, 0);
        }
    }
}

/** 只有真正"该跳的帧"才记账：用于背压降档 */
static void vp_add_pressure(void)
{
    s.pressure++;
    if (s.pressure < VP_PRESSURE_THRESH) {
        return;
    }
    s.pressure = 0;

    bool changed = false;
    uint8_t new_fps = s.fps_eff;
    uint8_t new_q = s.quality;

    if (new_fps > VP_FPS_MIN) {
        new_fps = (new_fps >= VP_FPS_MIN + 2) ? (uint8_t)(new_fps - 2) : VP_FPS_MIN;
        changed = true;
    }
    if (new_q > VP_QUALITY_MIN) {
        new_q = (new_q >= VP_QUALITY_MIN + 5) ? (uint8_t)(new_q - 5) : VP_QUALITY_MIN;
        changed = true;
    }
    if (!changed) {
        return;
    }
    s.fps_eff = new_fps;
    s.quality = new_q;
    s.last_recover_us = esp_timer_get_time();
    ESP_LOGW(TAG, "持续背压 → 降档：fps_eff=%u quality=%u（背压消失 %u s 后自动恢复）",
             (unsigned)s.fps_eff, (unsigned)s.quality, (unsigned)(VP_RECOVER_US / 1000000));
}

static void vp_recover(int64_t now)
{
    if (now - s.last_recover_us < VP_RECOVER_US) {
        return;
    }
    s.last_recover_us = now;
    if (s.fps_eff >= s.fps_target && s.quality >= s.quality_user) {
        return;
    }
    if (s.fps_eff < s.fps_target) {
        s.fps_eff++;
    }
    if (s.quality < s.quality_user) {
        s.quality = (uint8_t)((s.quality + 3 > s.quality_user) ? s.quality_user : (s.quality + 3));
    }
    ESP_LOGI(TAG, "背压消失 → 恢复：fps_eff=%u/%u quality=%u/%u",
             (unsigned)s.fps_eff, (unsigned)s.fps_target,
             (unsigned)s.quality, (unsigned)s.quality_user);
}

/** 把一份快照交给仪表盘（纯数值，不涉及 LVGL 调用） */
static void vp_push_ui_stats(void)
{
    ui_stats_t us;

    memset(&us, 0, sizeof(us));
    portENTER_CRITICAL(&s_mux);
    us.frames_out = s.stats.frames_out;
    us.frames_dropped = s.stats.frames_dropped;
    us.fps_x100 = s.stats.fps_actual_x100;
    us.encode_us_avg = s.stats.encode_us_avg;
    us.encode_us_max = s.stats.encode_us_max;
    us.render_us_avg = s.stats.render_us_avg;
    us.kbps = s.stats.kbps;
    us.bytes_last = s.stats.bytes_last;
    us.bytes_avg = s.stats.bytes_avg;
    portEXIT_CRITICAL(&s_mux);

    us.seq = s.frame_seq;
    us.fps_target = s.fps_eff;
    us.quality = s.quality;
    us.buffers = s.nbuf;
    us.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ui_update_stats(&us);
}

/** 只渲染不编码（无消费端 / 背压跳帧时用，保持 LVGL 时钟与动画活着） */
static void vp_render_only(void)
{
    ui_apply_cmds();
    uint32_t render_us = ui_render_once();
    vp_stats_add_render(render_us);
}

/** 把 ready 队列里没人要的"陈帧"退回空闲池（进入空闲状态时清一次） */
static void vp_drain_ready_frames(void)
{
    uint8_t stale = 0;
    while (xQueueReceive(s.ready_q, &stale, 0) == pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        s.slot[stale].state = SLOT_FREE;
        portEXIT_CRITICAL(&s_mux);
        vp_stats_drop(1);
        (void)xQueueSend(s.free_q, &stale, 0);
    }
}

/* ============================ 编码 ============================ */

static esp_err_t vp_encode(const uint8_t *rgb, uint32_t in_bytes, uint8_t *out, size_t out_cap,
                           uint32_t *out_len)
{
    jpeg_encode_cfg_t cfg = {
        .height = s.h,
        .width = s.w,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = VP_SUBSAMPLE,
        .image_quality = s.quality,
        .pixel_reverse = s.pixel_reverse,
    };
    uint32_t len = 0;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s.enc, &cfg, rgb, in_bytes, out, (uint32_t)out_cap, &len);
    int64_t dt = esp_timer_get_time() - t0;
    uint32_t us = (uint32_t)(dt < 0 ? 0 : dt);

    portENTER_CRITICAL(&s_mux);
    s.stats.encode_us_avg = ewma(s.stats.encode_us_avg, us);
    if (us > s.stats.encode_us_max) {
        s.stats.encode_us_max = us;
    }
    portEXIT_CRITICAL(&s_mux);

    if (err != ESP_OK) {
        return err;
    }
    *out_len = len;
    return ESP_OK;
}

/* ============================ 渲染 + 编码任务 ============================ */

static void vp_sleep_until(int64_t deadline_us)
{
    int64_t left = deadline_us - esp_timer_get_time();
    if (left <= 0) {
        return;
    }
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)((left + 999) / 1000));
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

static void vp_task(void *arg)
{
    (void)arg;

    while (!s.stopping) {
        int64_t now = esp_timer_get_time();

        vp_reclaim(now);
        vp_recover(now);
        vp_stats_window(now);

        int64_t cycle_start = now;
        int64_t period_us = 1000000 / (s.fps_eff ? s.fps_eff : VP_FPS_DEFAULT);

        /*
         * ---- 0) 有没有消费端？ ----
         * 判据是"最近一次 get_frame() 调用"，**含超时返回** —— 因为 usb_vendor 只有在
         * link 起来且 EP3 空闲时才会调它。这样：
         *   * 主机不在：get_frame 完全不发生 → 1.5 s 后进入空闲，只刷 UI，不编码、不占缓冲；
         *   * 主机回来：第一次 get_frame（哪怕超时）就把状态拉回"有消费端"，下一轮立刻产帧。
         * 日志按"状态变化 + 每 30 s 提醒一次"限流（之前每 ~220 ms 一条，会刷满串口）。
         */
        if (now - s.last_get_us > VP_IDLE_US) {
            if (!s.idle) {
                s.idle = true;
                vp_drain_ready_frames();   /* 陈帧扔回空闲池：主机回来时先看到新画面 */
                ESP_LOGW(TAG, "无消费端（>%lld ms 没有 get_frame）→ 暂停编码，只刷新 UI；"
                              "WebUSB 侧开始取帧后自动恢复",
                         (long long)(VP_IDLE_US / 1000));
                s.last_idle_log_us = now;
            } else if (now - s.last_idle_log_us >= VP_IDLE_LOG_US) {
                ESP_LOGW(TAG, "仍无消费端（已 %lld s）：UI 仍在刷新，编码保持暂停",
                         (long long)((now - s.last_get_us) / 1000000));
                s.last_idle_log_us = now;
            }
            vp_render_only();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s.idle) {
            s.idle = false;
            ESP_LOGI(TAG, "消费端恢复（get_frame 重新被调用）→ 恢复编码");
        }

        /* ---- 1) 背压债务：主动跳一帧（只刷 UI，不编码、不占缓冲） ---- */
        uint32_t debt = 0;
        portENTER_CRITICAL(&s_mux);
        if (s.drop_debt > 0) {
            debt = s.drop_debt;
            s.drop_debt--;
        }
        portEXIT_CRITICAL(&s_mux);

        if (debt > 0) {
            vp_stats_drop(1);
            vp_render_only();
            vp_sleep_until(cycle_start + period_us);
            continue;
        }

        /* ---- 2) 借一个输出缓冲 ---- */
        uint8_t idx = 0;
        if (xQueueReceive(s.free_q, &idx, 0) != pdTRUE) {
            /* 消费端在，但 3 个缓冲都被占着 → 真实背压 */
            vp_stats_drop(1);
            vp_add_pressure();
            vp_render_only();
            vp_sleep_until(cycle_start + period_us);
            continue;
        }

        /* 标记为"编码中"：release_frame 只认 BORROWED，杜绝误释放正在写的缓冲 */
        portENTER_CRITICAL(&s_mux);
        s.slot[idx].state = SLOT_ENCODING;
        portEXIT_CRITICAL(&s_mux);

        /* ---- 3) UI 命令 + 渲染 ---- */
        ui_apply_cmds();
        vp_push_ui_stats();
        uint32_t render_us = ui_render_once();
        vp_stats_add_render(render_us);

        /* ---- 4) 取刚才那次完整刷新的帧（零拷贝） ---- */
        const uint8_t *rgb = NULL;
        uint32_t rgb_bytes = 0;
        if (!ui_get_frame(&rgb, &rgb_bytes) || rgb == NULL) {
            /* 首帧还没渲染完（第一次全屏刷新可能要几十~几百 ms） */
            portENTER_CRITICAL(&s_mux);
            s.slot[idx].state = SLOT_FREE;
            portEXIT_CRITICAL(&s_mux);
            uint8_t back = idx;
            (void)xQueueSend(s.free_q, &back, 0);
            vp_sleep_until(cycle_start + period_us);
            continue;
        }

        /* ---- 5) 硬件 JPEG 编码 ---- */
        uint32_t seq = ++s.frame_seq;
        uint32_t ts_us = (uint32_t)esp_timer_get_time();
        uint32_t out_len = 0;
        esp_err_t err = vp_encode(rgb, rgb_bytes, s.slot[idx].data, s.slot[idx].cap, &out_len);

        if (err != ESP_OK) {
            s.enc_fail_count++;
            portENTER_CRITICAL(&s_mux);
            s.slot[idx].state = SLOT_FREE;
            portEXIT_CRITICAL(&s_mux);
            uint8_t back = idx;
            (void)xQueueSend(s.free_q, &back, 0);
            vp_stats_drop(1);
            vp_add_pressure();

            if (err == ESP_ERR_INVALID_STATE && s.quality > VP_QUALITY_MIN) {
                /* 输出缓冲装不下（质量过高/图像太复杂）：降质量重试下一帧 */
                s.quality = (uint8_t)((s.quality > VP_QUALITY_MIN + 10) ? (s.quality - 10) : VP_QUALITY_MIN);
                ESP_LOGW(TAG, "JPEG 输出超过缓冲（%u B）→ 自动降质量到 %u",
                         (unsigned)s.slot[idx].cap, (unsigned)s.quality);
            } else if ((s.enc_fail_count % 30u) == 1u) {
                ESP_LOGE(TAG, "JPEG 编码失败：%s（累计 %" PRIu32 " 次）",
                         esp_err_to_name(err), s.enc_fail_count);
            }
            vp_sleep_until(cycle_start + period_us);
            continue;
        }

        /* ---- 6) 发布到 ready 队列 ---- */
        portENTER_CRITICAL(&s_mux);
        s.slot[idx].len = out_len;
        s.slot[idx].seq = seq;
        s.slot[idx].ts_us = ts_us;
        s.slot[idx].state = SLOT_READY;
        s.stats.frames_out++;
        s.stats.bytes_last = out_len;
        s.stats.bytes_avg = ewma(s.stats.bytes_avg, out_len);
        portEXIT_CRITICAL(&s_mux);

        s.win_frames++;
        s.win_bytes += out_len;

        /* 背压是"漏桶"：每成功产出一帧漏掉 1 点压力。这样零星的 EP3 忙事件
         * （每次 report_drop(1)）永远不会累积成"降档"，只有持续背压才会。 */
        if (s.pressure > 0) {
            s.pressure--;
        }

        if (xQueueSend(s.ready_q, &idx, 0) != pdTRUE) {
            /* 理论上不可能（ready_q 深度 = nbuf，每个 slot 最多一次） */
            portENTER_CRITICAL(&s_mux);
            s.slot[idx].state = SLOT_FREE;
            portEXIT_CRITICAL(&s_mux);
            uint8_t back = idx;
            (void)xQueueSend(s.free_q, &back, 0);
            vp_stats_drop(1);
            ESP_LOGE(TAG, "ready 队列满，丢帧（内部状态异常）");
        }

        vp_sleep_until(cycle_start + period_us);
    }

    s.task = NULL;
    vTaskDelete(NULL);
}

/* ============================ 生命周期 ============================ */

void video_pipe_config_default(video_pipe_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->width = 1280;
    out->height = 720;
    out->fps = 30;
    out->fmt = WU_FMT_MJPEG;
    out->quality = VP_QUALITY_DEFAULT;
    out->overlay_stats = true;
    out->buffer_count = VP_DEFAULT_BUFS;
}

static void vp_teardown(void)
{
    if (s.enc != NULL) {
        (void)jpeg_del_encoder_engine(s.enc);
        s.enc = NULL;
    }
    for (uint8_t i = 0; i < VP_MAX_BUFS; i++) {
        if (s.slot[i].data != NULL) {
            free(s.slot[i].data);
            s.slot[i].data = NULL;
        }
        s.slot[i].state = SLOT_FREE;
        s.slot[i].len = 0;
        s.slot[i].cap = 0;
    }
    ui_deinit();
}

esp_err_t video_pipe_start(const video_pipe_config_t *cfg)
{
    if (s.started) {
        return ESP_OK;
    }

    video_pipe_config_t c;
    if (cfg != NULL) {
        c = *cfg;
    } else {
        video_pipe_config_default(&c);
    }

    /* ---- 配置夹取（不静默降级：改了就打 WARN） ---- */
    if (c.width < 64 || c.height < 64) {
        ESP_LOGE(TAG, "分辨率非法 %ux%u", (unsigned)c.width, (unsigned)c.height);
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t w = (uint16_t)(c.width & ~0x0Fu);
    uint16_t h = (uint16_t)(c.height & ~0x0Fu);
    if (w != c.width || h != c.height) {
        ESP_LOGW(TAG, "分辨率 %ux%u 不是 16 的倍数（JPEG MCU），向下取整为 %ux%u",
                 (unsigned)c.width, (unsigned)c.height, (unsigned)w, (unsigned)h);
    }
    uint8_t fps = c.fps;
    if (fps == 0) {
        fps = VP_FPS_DEFAULT;
    }
    if (fps < VP_FPS_MIN) {
        fps = VP_FPS_MIN;
    }
    if (fps > VP_FPS_MAX) {
        fps = VP_FPS_MAX;
    }
    uint8_t quality = (c.quality == 0) ? VP_QUALITY_DEFAULT : c.quality;
    if (quality > 100) {
        quality = 100;
    }
    if (quality < 1) {
        quality = 1;
    }
    uint8_t nbuf = c.buffer_count ? c.buffer_count : VP_DEFAULT_BUFS;
    if (nbuf < VP_MIN_BUFS) {
        nbuf = VP_MIN_BUFS;
    }
    if (nbuf > VP_MAX_BUFS) {
        nbuf = VP_MAX_BUFS;
    }
    if (c.fmt != WU_FMT_MJPEG) {
        ESP_LOGE(TAG, "起始格式 %d 不支持：本版只实现 MJPEG（H.264 阶段二；RGB565 未编译进固件）",
                 (int)c.fmt);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 队列跨 stop/start 复用（删除队列会和"还阻塞在 receive 里的消费者"竞争），这里只清内容 */
    QueueHandle_t free_q = s.free_q;
    QueueHandle_t ready_q = s.ready_q;
    memset(&s, 0, sizeof(s));
    s.free_q = free_q;
    s.ready_q = ready_q;
    if (s.free_q != NULL) {
        (void)xQueueReset(s.free_q);
    }
    if (s.ready_q != NULL) {
        (void)xQueueReset(s.ready_q);
    }

    s.w = w;
    s.h = h;
    s.nbuf = nbuf;
    s.quality = quality;
    s.quality_user = quality;
    s.fps_target = fps;
    s.fps_eff = fps;
    s.jpeg_timeout_ms = VP_ENC_TIMEOUT_MS;
    s.pixel_reverse = VP_PIXEL_REVERSE;
    s.fmt = WU_FMT_MJPEG;
    int64_t t0_us = esp_timer_get_time();
    s.last_get_us = t0_us;
    s.last_recover_us = t0_us;
    s.win_start_us = t0_us;

    /* ---- 输出帧缓冲：必须用 jpeg_alloc_encoder_mem（cache line 对齐 + PSRAM） ---- */
    s.out_cap = (size_t)w * (size_t)h + 64u * 1024u;   /* q100 也足够（720p ≈ 983 KiB） */
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    for (uint8_t i = 0; i < nbuf; i++) {
        size_t got = 0;
        s.slot[i].data = (uint8_t *)jpeg_alloc_encoder_mem(s.out_cap, &mem_cfg, &got);
        if (s.slot[i].data == NULL) {
            ESP_LOGE(TAG, "JPEG 输出缓冲 #%u 分配失败（请求 %u KiB，PSRAM free=%u KiB）",
                     (unsigned)i, (unsigned)(s.out_cap / 1024u),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
            vp_teardown();
            return ESP_ERR_NO_MEM;
        }
        s.slot[i].cap = got;
        s.slot[i].state = SLOT_FREE;
        s.out_alloc = got;
    }

    /* ---- 硬件 JPEG 引擎 ---- */
    jpeg_encode_engine_cfg_t eng_cfg = {
        .timeout_ms = (int)VP_ENC_TIMEOUT_MS,
    };
    esp_err_t err = jpeg_new_encoder_engine(&eng_cfg, &s.enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine 失败：%s", esp_err_to_name(err));
        vp_teardown();
        return err;
    }

    /* ---- LVGL 离屏显示（失败也不崩：返回错误，由 Lead 综合判定整机可用性） ---- */
    err = ui_init(w, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui_init 失败：%s — 没有渲染源，视频管线不启动", esp_err_to_name(err));
        vp_teardown();
        return err;
    }

    /* ---- 队列（跨 stop/start 复用，避免和还在阻塞的消费者竞争） ---- */
    if (s.free_q == NULL) {
        s.free_q = xQueueCreate(VP_MAX_BUFS, sizeof(uint8_t));
    }
    if (s.ready_q == NULL) {
        s.ready_q = xQueueCreate(VP_MAX_BUFS, sizeof(uint8_t));
    }
    if (s.free_q == NULL || s.ready_q == NULL) {
        ESP_LOGE(TAG, "帧队列创建失败");
        vp_teardown();
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < nbuf; i++) {
        (void)xQueueSend(s.free_q, &i, 0);
    }

    s.stopping = false;
    s.started = true;
    if (xTaskCreate(vp_task, "video_pipe", VP_TASK_STACK, NULL, VP_TASK_PRIO, &s.task) != pdPASS) {
        ESP_LOGE(TAG, "渲染任务创建失败");
        s.started = false;
        vp_teardown();
        return ESP_ERR_NO_MEM;
    }

    /* 真机核对用：一条 WARN 把关键数字全打出来（分辨率/质量/timeout/缓冲/PSRAM） */
    ESP_LOGW(TAG, "视频管线已启动（真实管线，非占位）：%ux%u RGB565 -> MJPEG q%u 4:2:0，target=%u fps，bufs=%u",
             (unsigned)w, (unsigned)h, (unsigned)s.quality, (unsigned)fps, (unsigned)nbuf);
    ESP_LOGW(TAG, "编码器：timeout=%" PRIu32 " ms，pixel_reverse=%d，单帧输出上限=%u KiB/实际分配=%u KiB",
             s.jpeg_timeout_ms, (int)s.pixel_reverse,
             (unsigned)(s.out_cap / 1024u), (unsigned)(s.out_alloc / 1024u));
    ESP_LOGW(TAG, "帧缓冲：LVGL 双缓冲 %u KiB×2 + JPEG 输出 %u KiB×%u = %u KiB；PSRAM free=%u KiB / total=%u KiB",
             (unsigned)((size_t)w * h * 2u / 1024u),
             (unsigned)(s.out_alloc / 1024u), (unsigned)nbuf,
             (unsigned)(((size_t)w * h * 2u * 2u + s.out_alloc * nbuf) / 1024u),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024u));
    return ESP_OK;
}

esp_err_t video_pipe_stop(void)
{
    if (!s.started) {
        return ESP_OK;
    }
    s.stopping = true;

    /* 等渲染任务自己退出（它每轮最多睡眠一个帧周期；空闲路径是 100 ms） */
    for (int i = 0; i < 300 && s.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s.task != NULL) {
        ESP_LOGE(TAG, "渲染任务未在 3 s 内退出（%p）", (void *)s.task);
        vTaskDelete(s.task);
        s.task = NULL;
    }

    s.started = false;
    vp_teardown();
    ESP_LOGI(TAG, "视频管线已停止");
    return ESP_OK;
}

/* ============================ 运行时配置 ============================ */

esp_err_t video_pipe_set_format(wu_frame_fmt_t fmt)
{
    if (fmt == WU_FMT_MJPEG) {
        s.fmt = WU_FMT_MJPEG;
        return ESP_OK;
    }
    /* 帧边界切换做不到"一半 MJPEG 一半别的"：本版只有 MJPEG 编码器 */
    ESP_LOGW(TAG, "set_format(%d)：本版只支持 MJPEG（H.264 属阶段二；RGB565 未编译进固件）", (int)fmt);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t video_pipe_set_quality(uint8_t quality)
{
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    s.quality_user = quality;
    s.quality = quality;
    portEXIT_CRITICAL(&s_mux);
    s.last_recover_us = esp_timer_get_time();
    ESP_LOGI(TAG, "JPEG 质量改为 %u（下一帧生效）", (unsigned)quality);
    return ESP_OK;
}

/* ============================ I2 帧源 ============================ */

esp_err_t video_pipe_get_frame(video_frame_t *out, uint32_t timeout_ms)
{
    if (!s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 关键：**每次调用**都算"有消费端"，包括马上要返回超时的情况。
     * 渲染任务用这个时间戳判断"该不该继续编码"（见 vp_task 的 0) 段）。 */
    s.last_get_us = esp_timer_get_time();

    uint8_t idx = 0;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s.ready_q, &idx, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memset(out, 0, sizeof(*out));

    portENTER_CRITICAL(&s_mux);
    vp_slot_t *sl = &s.slot[idx];
    if (sl->state != SLOT_READY) {
        /* 已被兜底回收（借出超时/内部状态变化）：当作没有新帧 */
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_TIMEOUT;
    }
    sl->state = SLOT_BORROWED;
    sl->borrow_us = esp_timer_get_time();

    out->data = sl->data;
    out->len = sl->len;
    out->frame_seq = sl->seq;
    out->ts_us = sl->ts_us;
    out->fmt = s.fmt;
    out->width = s.w;
    out->height = s.h;
    out->keyframe = true;   /* MJPEG：每个 JPEG 都是 I 帧 */
    portEXIT_CRITICAL(&s_mux);

    return ESP_OK;
}

void video_pipe_release_frame(video_frame_t *frame)
{
    if (frame == NULL || frame->data == NULL || !s.started) {
        return;
    }

    uint8_t idx = 0xFF;
    bool released = false;

    portENTER_CRITICAL(&s_mux);
    for (uint8_t i = 0; i < s.nbuf; i++) {
        if (s.slot[i].data == frame->data) {
            idx = i;
            if (s.slot[i].state == SLOT_BORROWED) {
                s.slot[i].state = SLOT_FREE;
                released = true;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    frame->data = NULL;
    frame->len = 0;

    if (!released) {
        ESP_LOGW(TAG, "release_frame：指针不属于本管线或状态不是 BORROWED（忽略）");
        return;
    }
    (void)xQueueSend(s.free_q, &idx, 0);
}

bool video_pipe_frame_ready(void)
{
    if (!s.started || s.ready_q == NULL) {
        return false;
    }
    return uxQueueMessagesWaiting(s.ready_q) > 0;
}

void video_pipe_report_drop(uint32_t dropped)
{
    if (!s.started || dropped == 0) {
        return;
    }
    uint32_t add = (dropped > 8u) ? 8u : dropped;

    portENTER_CRITICAL(&s_mux);
    s.drop_debt += add;
    if (s.drop_debt > VP_DROP_DEBT_MAX) {
        s.drop_debt = VP_DROP_DEBT_MAX;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ============================ UI 命令 ============================ */

/**
 * 本管线自己的两个调参键（真机调色/调质量用，不需要重烧）：
 *   `pixrev=0|1`  RGB565 字节序（花屏或红蓝互换时翻一下）
 *   `quality=1..100`
 * 其余 key=value 原样转给 UI（title/theme/page/text/overlay）。
 */
esp_err_t video_pipe_post_ui_cmd(const char *utf8, size_t len)
{
    char local[128];
    char fwd[128];
    size_t fwd_n = 0;

    if (!s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (utf8 == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = (len < sizeof(local) - 1) ? len : sizeof(local) - 1;
    memcpy(local, utf8, n);
    local[n] = '\0';
    fwd[0] = '\0';

    char *p = local;
    while (p != NULL && *p != '\0') {
        char *sep = strpbrk(p, ";&,");
        if (sep != NULL) {
            *sep = '\0';
            sep++;
        }
        char *eq = strchr(p, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char *k = p;
            const char *v = eq + 1;
            if (strcmp(k, "pixrev") == 0) {
                s.pixel_reverse = (atoi(v) != 0);
                ESP_LOGW(TAG, "RGB565 pixel_reverse=%d（下一帧生效；红蓝互换/花屏时切换）",
                         (int)s.pixel_reverse);
                p = sep;
                continue;
            }
            if (strcmp(k, "quality") == 0) {
                int q = atoi(v);
                if (q >= 1 && q <= 100) {
                    video_pipe_set_quality((uint8_t)q);
                } else {
                    ESP_LOGW(TAG, "quality=%d 超出 1..100，忽略", q);
                }
                p = sep;
                continue;
            }
            *eq = '=';
        }
        /* 不是本管线的键：拼回转发串 */
        size_t tl = strlen(p);
        if (fwd_n + tl + 2 < sizeof(fwd)) {
            if (fwd_n != 0) {
                fwd[fwd_n++] = ';';
            }
            memcpy(fwd + fwd_n, p, tl);
            fwd_n += tl;
            fwd[fwd_n] = '\0';
        }
        p = sep;
    }

    if (fwd_n == 0) {
        return ESP_OK;
    }
    return ui_post_cmd(fwd, fwd_n);
}

/* ============================ 统计 ============================ */

void video_pipe_get_stats(video_pipe_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s.stats;
    portEXIT_CRITICAL(&s_mux);
}
