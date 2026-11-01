// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · UI（LVGL 9.2 离屏显示 + 仪表盘）—— 实现
//
// 设计要点（为什么这么写，写给下一个改它的人）：
//   * **没有屏幕**：1280x720 RGB565 的显示只是一块 PSRAM。flush_cb 不拷贝任何东西，
//     只记下"这次完整刷新的帧缓冲首地址"并立刻 `lv_display_flush_ready()`。
//   * **DIRECT + 双缓冲**：LVGL 9.2 在 DIRECT 模式下只重画"脏区"，并在下一次刷新前
//     用 `lv_draw_buf_copy` 把另一块缓冲的对应区域同步过来 —— 所以每次最后 flush
//     之后，那块缓冲一定是一张**完整**的 1280x720 图像（而不是带残影的脏区图）。
//     这正是硬件 JPEG 可以直接吃这块内存的前提（零拷贝）。
//   * **tick**：`lv_tick_set_cb(esp_timer)`，不需要额外的 esp_timer 周期回调。
//   * **线程**：所有 LVGL 调用只在 video_pipe 的渲染任务里发生（含 flush_cb）。
//     `ui_post_cmd()` 是唯一允许从别的任务调用的入口（只写 FreeRTOS 队列）。
//   * **字体**：只用 ASCII（LVGL 内置 Montserrat），不引入中文字库；字号按 Kconfig
//     逐级降级，最差也能编译/运行在只有 montserrat_14 的配置上。
//
// 写入者：WS-V（video_pipe / ui）。

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "sd_storage.h"   /* 挂载状态 + 容量（FATFS 的 VFS 没有 statvfs，见 ui_update_sd） */
#include "ui.h"

static const char *TAG = "ui";

/* ============================ 常量 ============================ */

#define UI_DRAW_ALIGN         64u
#define UI_CMD_LEN            128
#define UI_CMD_DEPTH          8
#define UI_SEG_N              16

#define UI_REFRESH_SLOW_US    (500 * 1000)      /* 统计文字/慢变行 */
#define UI_REFRESH_CLOCK_US   (500 * 1000)      /* 墙钟 */
#define UI_REFRESH_SD_US      (2 * 1000 * 1000) /* SD 容量 */

/* 1980 之前的 time() 视为"没有 RTC/NTP"：ESP32 上电默认 1970 */
#define UI_TIME_VALID_MIN     1735689600        /* 2025-01-01T00:00:00Z */

/* ============================ 字体（逐级降级） ============================ */

#if LV_FONT_MONTSERRAT_48
#define UI_FONT_TITLE   (&lv_font_montserrat_48)
#elif LV_FONT_MONTSERRAT_28
#define UI_FONT_TITLE   (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_20
#define UI_FONT_TITLE   (&lv_font_montserrat_20)
#else
#define UI_FONT_TITLE   (&lv_font_montserrat_14)
#endif

#if LV_FONT_MONTSERRAT_28
#define UI_FONT_BIG     (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_20
#define UI_FONT_BIG     (&lv_font_montserrat_20)
#else
#define UI_FONT_BIG     (&lv_font_montserrat_14)
#endif

#if LV_FONT_MONTSERRAT_20
#define UI_FONT_MID     (&lv_font_montserrat_20)
#elif LV_FONT_MONTSERRAT_16
#define UI_FONT_MID     (&lv_font_montserrat_16)
#else
#define UI_FONT_MID     (&lv_font_montserrat_14)
#endif

#if LV_FONT_MONTSERRAT_16
#define UI_FONT_SMALL   (&lv_font_montserrat_16)
#else
#define UI_FONT_SMALL   (&lv_font_montserrat_14)
#endif

/* ============================ 主题 ============================ */

typedef struct {
    uint32_t bg;      /* 整屏背景 */
    uint32_t panel;   /* 顶栏 / 条底 */
    uint32_t text;    /* 正文 */
    uint32_t dim;     /* 次要文字 */
    uint32_t accent;  /* 强调 */
    uint32_t warn;    /* 告警 / 动态块 */
} ui_palette_t;

static const ui_palette_t PAL_DARK = {
    .bg = 0x0B0F14, .panel = 0x151E29, .text = 0xE7F0F8,
    .dim = 0x8CA0B3, .accent = 0x3FB6F0, .warn = 0xFFB74D,
};
static const ui_palette_t PAL_LIGHT = {
    .bg = 0xEEF3F8, .panel = 0xFFFFFF, .text = 0x101820,
    .dim = 0x5B6C7C, .accent = 0x0B78C2, .warn = 0xD97706,
};

/* ============================ 状态 ============================ */

static struct {
    bool        ready;
    uint16_t    w;
    uint16_t    h;
    uint32_t    buf_bytes;
    uint8_t    *buf[2];

    lv_display_t *disp;

    /* 最近一次"完整刷新"的帧缓冲（flush_cb 里写，video_pipe 读） */
    volatile const uint8_t *frame;
    volatile uint32_t       frame_gen;

    int64_t     t0_us;

    /* 仪表盘控件 */
    lv_obj_t *scr;
    lv_obj_t *header;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_clock;
    lv_obj_t *lbl_uptime;
    lv_obj_t *lbl_sd;
    lv_obj_t *lbl_fast;
    lv_obj_t *lbl_slow;
    lv_obj_t *lbl_seq;
    lv_obj_t *lbl_footer;

    lv_obj_t *page_main;
    lv_obj_t *page_stats;
    lv_obj_t *seg[UI_SEG_N];
    lv_obj_t *ind;
    lv_obj_t *bar;
    lv_obj_t *lbl_dyn;
    lv_obj_t *lbl_stats_big;
    lv_obj_t *lbl_note;

    /* 慢变文字缓存（避免每帧都 invalidate 大块区域） */
    char        txt_slow[256];
    char        txt_fast[192];
    char        txt_stats_big[384];
    uint32_t    shown_fps_x100;
    uint32_t    shown_enc;
    uint32_t    shown_render;
    uint32_t    shown_kbps;
    uint32_t    shown_seq;
    uint32_t    shown_frames;
    uint32_t    shown_drops;
    uint32_t    shown_bytes_last;
    uint32_t    shown_bytes_avg;
    uint32_t    shown_quality;
    uint32_t    shown_target;
    uint32_t    shown_psram;
    uint32_t    render_count;

    ui_stats_t  snap;
    bool        has_snap;

    ui_palette_t pal;
    bool         theme_dark;

    QueueHandle_t cmd_q;
    int64_t      last_clock_us;
    int64_t      last_slow_us;
    int64_t      last_sd_us;
} s;

/* ============================ 小工具 ============================ */

static uint32_t ui_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);   /* us -> ms，32 位回绕由 LVGL 处理 */
}

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    /* DIRECT 模式下 px_map 恒等于整块帧缓冲的首地址（stride = w*2，无 padding） */
    s.frame = px_map;
    s.frame_gen++;
    lv_display_flush_ready(disp);
}

static lv_obj_t *mk_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t color, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

/** 只有文字真的变了才 set（减少无效的脏区） */
static bool set_text_if_changed(lv_obj_t *lbl, char *cache, size_t cap, const char *fmt, ...)
{
    char buf[384];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return false;
    }
    if (cache != NULL && strncmp(cache, buf, cap) == 0) {
        return false;
    }
    if (cache != NULL) {
        strncpy(cache, buf, cap - 1);
        cache[cap - 1] = '\0';
    }
    lv_label_set_text(lbl, buf);
    return true;
}

/* ============================ 主题应用 ============================ */

static void ui_apply_theme(void)
{
    const ui_palette_t *p = &s.pal;

    lv_obj_set_style_bg_color(s.scr, lv_color_hex(p->bg), 0);
    lv_obj_set_style_bg_opa(s.scr, LV_OPA_COVER, 0);

    lv_obj_set_style_bg_color(s.header, lv_color_hex(p->panel), 0);

    lv_obj_set_style_text_color(s.lbl_title, lv_color_hex(p->accent), 0);
    lv_obj_set_style_text_color(s.lbl_clock, lv_color_hex(p->text), 0);
    lv_obj_set_style_text_color(s.lbl_uptime, lv_color_hex(p->text), 0);
    lv_obj_set_style_text_color(s.lbl_sd, lv_color_hex(p->text), 0);
    lv_obj_set_style_text_color(s.lbl_fast, lv_color_hex(p->accent), 0);
    lv_obj_set_style_text_color(s.lbl_slow, lv_color_hex(p->dim), 0);
    lv_obj_set_style_text_color(s.lbl_seq, lv_color_hex(p->text), 0);
    lv_obj_set_style_text_color(s.lbl_footer, lv_color_hex(p->dim), 0);
    lv_obj_set_style_text_color(s.lbl_dyn, lv_color_hex(p->warn), 0);
    lv_obj_set_style_text_color(s.lbl_stats_big, lv_color_hex(p->text), 0);
    lv_obj_set_style_text_color(s.lbl_note, lv_color_hex(p->dim), 0);

    lv_obj_set_style_bg_color(s.bar, lv_color_hex(p->panel), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(p->accent), LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(s.ind, lv_color_hex(p->warn), 0);

    /* 主题切换是低频动作：整屏重绘一次，避免残留旧配色 */
    lv_obj_invalidate(s.scr);
}

/* ============================ 仪表盘 ============================ */

static void ui_build_dashboard(void)
{
    s.scr = lv_screen_active();
    lv_obj_remove_style_all(s.scr);
    lv_obj_set_style_bg_opa(s.scr, LV_OPA_COVER, 0);

    /* --- 顶栏 --- */
    s.header = mk_rect(s.scr, 0, 0, (int32_t)s.w, 104, s.pal.panel, 0);
    s.lbl_title = mk_label(s.header, UI_FONT_TITLE, s.pal.accent, "MeidaP4");
    lv_obj_set_pos(s.lbl_title, 32, 12);
    s.lbl_clock = mk_label(s.header, UI_FONT_MID, s.pal.text, "CLOCK ...");
    lv_obj_align(s.lbl_clock, LV_ALIGN_TOP_RIGHT, -32, 34);

    /* --- 固定信息行 --- */
    s.lbl_uptime = mk_label(s.scr, UI_FONT_MID, s.pal.text, "Uptime 00:00:00");
    lv_obj_set_pos(s.lbl_uptime, 32, 120);

    s.lbl_sd = mk_label(s.scr, UI_FONT_MID, s.pal.text, "SD /sdcard  ...");
    lv_obj_set_pos(s.lbl_sd, 32, 154);

    s.lbl_fast = mk_label(s.scr, UI_FONT_MID, s.pal.accent, "FPS --   ENC --   RENDER --   LINK -- kbps");
    lv_obj_set_pos(s.lbl_fast, 32, 190);
    lv_obj_set_width(s.lbl_fast, (int32_t)s.w - 64);
    lv_label_set_long_mode(s.lbl_fast, LV_LABEL_LONG_CLIP);

    s.lbl_slow = mk_label(s.scr, UI_FONT_SMALL, s.pal.dim, "stats ...");
    lv_obj_set_pos(s.lbl_slow, 32, 228);
    lv_obj_set_width(s.lbl_slow, (int32_t)s.w - 64);
    lv_label_set_long_mode(s.lbl_slow, LV_LABEL_LONG_CLIP);

    /* --- 主页面：肉眼判断"没冻结"的动态元素 --- */
    s.page_main = lv_obj_create(s.scr);
    lv_obj_remove_style_all(s.page_main);
    lv_obj_set_pos(s.page_main, 0, 286);
    lv_obj_set_size(s.page_main, (int32_t)s.w, 380);
    lv_obj_remove_flag(s.page_main, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < UI_SEG_N; i++) {
        s.seg[i] = mk_rect(s.page_main, 32 + i * 56, 12, 48, 64, 0x203040, 6);
    }
    s.ind = mk_rect(s.page_main, 32, 96, 56, 56, s.pal.warn, 10);

    s.bar = lv_bar_create(s.page_main);
    lv_obj_set_pos(s.bar, 32, 176);
    lv_obj_set_size(s.bar, 780, 26);
    lv_bar_set_range(s.bar, 0, 100);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(s.pal.panel), LV_PART_MAIN);
    lv_obj_set_style_radius(s.bar, 13, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(s.pal.accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s.bar, 13, LV_PART_INDICATOR);

    s.lbl_dyn = mk_label(s.page_main, UI_FONT_BIG, s.pal.warn, "Hello MeidaP4");
    lv_obj_set_pos(s.lbl_dyn, 32, 240);
    lv_obj_set_width(s.lbl_dyn, (int32_t)s.w - 64);
    lv_label_set_long_mode(s.lbl_dyn, LV_LABEL_LONG_CLIP);

    /* --- 第二页：更详细的统计（page=stats） --- */
    s.page_stats = lv_obj_create(s.scr);
    lv_obj_remove_style_all(s.page_stats);
    lv_obj_set_pos(s.page_stats, 0, 286);
    lv_obj_set_size(s.page_stats, (int32_t)s.w, 380);
    lv_obj_add_flag(s.page_stats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.page_stats, LV_OBJ_FLAG_SCROLLABLE);

    s.lbl_stats_big = mk_label(s.page_stats, UI_FONT_MID, s.pal.text, "DIAGNOSTICS");
    lv_obj_set_pos(s.lbl_stats_big, 32, 8);
    lv_label_set_long_mode(s.lbl_stats_big, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s.lbl_stats_big, (int32_t)s.w - 64);

    s.lbl_note = mk_label(s.page_stats, UI_FONT_SMALL, s.pal.dim,
                          "page=main to go back  |  cmds: title= theme=dark|light text= overlay=0|1");
    lv_obj_set_pos(s.lbl_note, 32, 300);

    /* --- 底部 --- */
    s.lbl_seq = mk_label(s.scr, UI_FONT_MID, s.pal.text, "SEQ 000000");
    lv_obj_align(s.lbl_seq, LV_ALIGN_BOTTOM_RIGHT, -32, -28);

    s.lbl_footer = mk_label(s.scr, UI_FONT_SMALL, s.pal.dim, "");
    lv_obj_set_pos(s.lbl_footer, 32, (int32_t)s.h - 40);
    set_text_if_changed(s.lbl_footer, NULL, 0,
                        "OFFSCREEN %ux%u RGB565  |  DIRECT double-buffer  |  UI cmds: title/theme/page/text/overlay",
                        (unsigned)s.w, (unsigned)s.h);
}

/* ============================ 慢变文字 ============================ */

static void ui_update_clock(void)
{
    char buf[64];
    time_t now = time(NULL);
    if (now >= (time_t)UI_TIME_VALID_MIN) {
        struct tm tmv;
        char tb[32];
        gmtime_r(&now, &tmv);
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
        snprintf(buf, sizeof(buf), "UTC %s", tb);
    } else {
        snprintf(buf, sizeof(buf), "CLOCK not set (no RTC/NTP)");
    }
    if (set_text_if_changed(s.lbl_clock, NULL, 0, "%s", buf)) {
        lv_obj_align(s.lbl_clock, LV_ALIGN_TOP_RIGHT, -32, 34);
    }
}

static void ui_update_uptime(void)
{
    uint64_t up_s = (uint64_t)((esp_timer_get_time() - s.t0_us) / 1000000);
    uint32_t days = (uint32_t)(up_s / 86400u);
    uint32_t hh = (uint32_t)((up_s / 3600u) % 24u);
    uint32_t mm = (uint32_t)((up_s / 60u) % 60u);
    uint32_t ss = (uint32_t)(up_s % 60u);

    if (days > 0) {
        set_text_if_changed(s.lbl_uptime, NULL, 0, "Uptime %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                            days, hh, mm, ss);
    } else {
        set_text_if_changed(s.lbl_uptime, NULL, 0, "Uptime %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hh, mm, ss);
    }
}

static void ui_update_sd(void)
{
    uint64_t total = 0;
    uint64_t free_b = 0;
    bool mounted = sd_storage_is_mounted();
    const char *mp = sd_storage_mount_point();   /* 挂载点的唯一来源（sd_storage 的 Kconfig） */
    char buf[128];

    /*
     * ⚠️ 不能用 `statvfs()`：ESP-IDF 的 FATFS VFS **没有**实现 statvfs（vfs_fat.c 里只有
     * esp_vfs_fat_info），statvfs 恒返回 -1/ENOSYS —— 真机上就表现成"卡明明挂着却显示
     * not mounted"。容量必须走 sd_storage_get_capacity()（内部 esp_vfs_fat_info）。
     */
    if (mounted) {
        esp_err_t err = sd_storage_get_capacity(&total, &free_b);
        uint64_t frs = (uint64_t)(1024u * 1024u);
        uint32_t avail_mib = (err == ESP_OK && total > 0) ? (uint32_t)(free_b / frs) : 0u;
        uint32_t total_mib = (err == ESP_OK && total > 0) ? (uint32_t)(total / frs) : 0u;

        if (total_mib >= 1024u) {
            snprintf(buf, sizeof(buf), "SD %s  %" PRIu32 ".%" PRIu32 " / %" PRIu32 ".%" PRIu32 " GiB free",
                     mp, avail_mib / 1024u, (avail_mib % 1024u) * 10u / 1024u,
                     total_mib / 1024u, (total_mib % 1024u) * 10u / 1024u);
        } else if (total_mib > 0u) {
            snprintf(buf, sizeof(buf), "SD %s  %" PRIu32 " / %" PRIu32 " MiB free", mp, avail_mib, total_mib);
        } else {
            /* 已挂载但取不到容量（卡刚拔出/FATFS 忙）：明确区分于"未挂载" */
            snprintf(buf, sizeof(buf), "SD %s  mounted (size n/a)", mp);
        }
    } else {
        snprintf(buf, sizeof(buf), "SD %s  not mounted (n/a)", mp);
    }
    set_text_if_changed(s.lbl_sd, NULL, 0, "%s", buf);
}

static void ui_update_slow(void)
{
    const ui_stats_t *st = &s.snap;

    set_text_if_changed(s.lbl_slow, s.txt_slow, sizeof(s.txt_slow),
                        "FRAMES %" PRIu32 " out / %" PRIu32 " drop | BYTES %" PRIu32 " KiB last / %" PRIu32
                        " KiB avg | QUALITY %u | TARGET %" PRIu32 " fps | BUFS %u\n"
                        "SEQ %06" PRIu32 " | ENC %" PRIu32 ".%" PRIu32 "/%" PRIu32 ".%" PRIu32 " ms | RENDER %"
                        PRIu32 ".%" PRIu32 " ms | PSRAM free %" PRIu32 " KiB",
                        st->frames_out, st->frames_dropped,
                        st->bytes_last / 1024u, st->bytes_avg / 1024u,
                        (unsigned)st->quality, st->fps_target, (unsigned)st->buffers,
                        st->seq,
                        st->encode_us_avg / 1000u, (st->encode_us_avg % 1000u) / 100u,
                        st->encode_us_max / 1000u, (st->encode_us_max % 1000u) / 100u,
                        st->render_us_avg / 1000u, (st->render_us_avg % 1000u) / 100u,
                        st->psram_free / 1024u);

    set_text_if_changed(s.lbl_stats_big, s.txt_stats_big, sizeof(s.txt_stats_big),
                        "DIAGNOSTICS  %" PRIu32 "x%" PRIu32 " MJPEG\n\n"
                        "frames_out      %" PRIu32 "\n"
                        "frames_dropped  %" PRIu32 "\n"
                        "fps_actual      %" PRIu32 ".%02" PRIu32 "  (target %" PRIu32 ")\n"
                        "encode_us       avg %" PRIu32 "  max %" PRIu32 "\n"
                        "render_us       avg %" PRIu32 "\n"
                        "bytes           last %" PRIu32 "  avg %" PRIu32 "\n"
                        "kbps            %" PRIu32 "\n"
                        "quality         %u\n"
                        "frame_seq       %" PRIu32 "\n"
                        "psram_free      %" PRIu32 " KiB",
                        (unsigned)s.w, (unsigned)s.h,
                        st->frames_out, st->frames_dropped,
                        st->fps_x100 / 100u, st->fps_x100 % 100u, st->fps_target,
                        st->encode_us_avg, st->encode_us_max,
                        st->render_us_avg,
                        st->bytes_last, st->bytes_avg,
                        st->kbps, (unsigned)st->quality, st->seq,
                        st->psram_free / 1024u);

    s.shown_frames = st->frames_out;
    s.shown_drops = st->frames_dropped;
    s.shown_bytes_last = st->bytes_last;
    s.shown_bytes_avg = st->bytes_avg;
    s.shown_quality = st->quality;
    s.shown_target = st->fps_target;
    s.shown_psram = st->psram_free;
}

/* ============================ 每帧动态 ============================ */

static void ui_update_dynamic(void)
{
    const ui_stats_t *st = &s.snap;

    s.render_count++;

    /* 快变行：每帧都比一次，只有内容变了才 invalidate */
    set_text_if_changed(s.lbl_fast, s.txt_fast, sizeof(s.txt_fast),
                        "FPS %" PRIu32 ".%02" PRIu32 " (t%" PRIu32 ")   ENC %" PRIu32 ".%" PRIu32
                        " ms   RENDER %" PRIu32 ".%" PRIu32 " ms   LINK %" PRIu32 " kbps",
                        st->fps_x100 / 100u, st->fps_x100 % 100u, st->fps_target,
                        st->encode_us_avg / 1000u, (st->encode_us_avg % 1000u) / 100u,
                        st->render_us_avg / 1000u, (st->render_us_avg % 1000u) / 100u,
                        st->kbps);

    /* 帧序号：视频是否在推进，肉眼一眼可见 */
    if (st->seq != s.shown_seq) {
        set_text_if_changed(s.lbl_seq, NULL, 0, "SEQ %06" PRIu32, st->seq);
        s.shown_seq = st->seq;
    }

    /* 动态色条：每一帧都改颜色 —— 主机画布上一眼就能判断"冻结 / 卡顿" */
    uint32_t phase = s.render_count;
    for (int i = 0; i < UI_SEG_N; i++) {
        uint16_t hue = (uint16_t)((phase * 3u + (uint32_t)i * 22u) % 360u);
        lv_obj_set_style_bg_color(s.seg[i], lv_color_hsv_to_rgb(hue, 85, 92), 0);
    }

    /* 移动方块 + 滚动条：与色条一起构成"活着"的三重证据 */
    {
        int32_t span = (int32_t)s.w - 64 - 56;
        int32_t x = 32 + (int32_t)((phase * 23u) % (uint32_t)span);
        lv_obj_set_x(s.ind, x);
    }
    lv_bar_set_value(s.bar, (int32_t)((phase * 7u) % 101u), LV_ANIM_OFF);
}

/* ============================ 对外 API ============================ */

esp_err_t ui_init(uint16_t width, uint16_t height)
{
    if (s.ready) {
        return ESP_OK;
    }
    if (width < 64 || height < 64) {
        ESP_LOGE(TAG, "分辨率太小：%ux%u", (unsigned)width, (unsigned)height);
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s, 0, sizeof(s));
    s.w = width;
    s.h = height;
    s.buf_bytes = (uint32_t)width * (uint32_t)height * 2u;
    s.pal = PAL_DARK;
    s.theme_dark = true;
    s.t0_us = esp_timer_get_time();
    s.snap.fps_target = 30;
    s.snap.quality = 80;
    s.snap.buffers = 3;

    /* --- PSRAM 帧缓冲（JPEG 驱动的输入要求 16B 对齐 + 非加密 PSRAM，这里给 64B） --- */
    s.buf[0] = heap_caps_aligned_alloc(UI_DRAW_ALIGN, s.buf_bytes, MALLOC_CAP_SPIRAM);
    s.buf[1] = heap_caps_aligned_alloc(UI_DRAW_ALIGN, s.buf_bytes, MALLOC_CAP_SPIRAM);
    if (s.buf[0] == NULL || s.buf[1] == NULL) {
        ESP_LOGE(TAG, "LVGL 帧缓冲分配失败：%ux%u RGB565 双缓冲需要 %u KiB ×2（PSRAM free=%u KiB）",
                 (unsigned)width, (unsigned)height, (unsigned)(s.buf_bytes / 1024u),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
        ui_deinit();
        return ESP_ERR_NO_MEM;
    }
    memset(s.buf[0], 0, s.buf_bytes);
    memset(s.buf[1], 0, s.buf_bytes);

    if (!lv_is_initialized()) {
        lv_init();
    }
    lv_tick_set_cb(ui_tick_cb);

    s.disp = lv_display_create(width, height);
    if (s.disp == NULL) {
        ESP_LOGE(TAG, "lv_display_create 失败");
        ui_deinit();
        return ESP_FAIL;
    }
    lv_display_set_color_format(s.disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s.disp, s.buf[0], s.buf[1], s.buf_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s.disp, ui_flush_cb);
    lv_display_set_default(s.disp);

    s.cmd_q = xQueueCreate(UI_CMD_DEPTH, UI_CMD_LEN);
    if (s.cmd_q == NULL) {
        ESP_LOGE(TAG, "UI 命令队列创建失败");
        ui_deinit();
        return ESP_ERR_NO_MEM;
    }

    ui_build_dashboard();
    ui_apply_theme();

    s.frame = NULL;
    s.frame_gen = 0;
    s.ready = true;

    ESP_LOGI(TAG, "LVGL %d.%d.%d 离屏显示就绪：%ux%u RGB565，DIRECT 双缓冲，每块 %u KiB（PSRAM，64B 对齐）",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             (unsigned)width, (unsigned)height, (unsigned)(s.buf_bytes / 1024u));
    if (lv_display_get_color_format(s.disp) != LV_COLOR_FORMAT_RGB565) {
        ESP_LOGE(TAG, "!! 显示颜色格式不是 RGB565（lv_conf 的 LV_COLOR_DEPTH 可能不是 16）");
    }
    return ESP_OK;
}

void ui_deinit(void)
{
    if (s.cmd_q != NULL) {
        vQueueDelete(s.cmd_q);
        s.cmd_q = NULL;
    }
    if (lv_is_initialized()) {
        if (s.disp != NULL) {
            /* 只删显示，不动别的全局状态（LVGL 在 9.x 里 lv_deinit 会清掉所有对象） */
            s.disp = NULL;
        }
        lv_deinit();
    }
    if (s.buf[0] != NULL) {
        heap_caps_free(s.buf[0]);
        s.buf[0] = NULL;
    }
    if (s.buf[1] != NULL) {
        heap_caps_free(s.buf[1]);
        s.buf[1] = NULL;
    }
    s.ready = false;
    s.frame = NULL;
}

bool ui_is_ready(void)
{
    return s.ready;
}

uint32_t ui_render_once(void)
{
    if (!s.ready) {
        return 0;
    }

    int64_t now = esp_timer_get_time();

    /* 慢变文字 / 墙钟 / SD 容量：分频更新，别每帧都重画大块区域 */
    if (s.has_snap && now - s.last_slow_us >= UI_REFRESH_SLOW_US) {
        s.last_slow_us = now;
        ui_update_slow();
    }
    if (now - s.last_clock_us >= UI_REFRESH_CLOCK_US) {
        s.last_clock_us = now;
        ui_update_clock();
        ui_update_uptime();
    }
    if (now - s.last_sd_us >= UI_REFRESH_SD_US) {
        s.last_sd_us = now;
        ui_update_sd();
    }

    ui_update_dynamic();

    int64_t t0 = esp_timer_get_time();
    uint32_t next = lv_timer_handler();
    int64_t dt = esp_timer_get_time() - t0;
    (void)next;

    return (uint32_t)(dt < 0 ? 0 : dt);
}

bool ui_get_frame(const uint8_t **rgb565, uint32_t *bytes)
{
    const uint8_t *p = (const uint8_t *)s.frame;
    if (!s.ready || p == NULL) {
        return false;
    }
    if (rgb565 != NULL) {
        *rgb565 = p;
    }
    if (bytes != NULL) {
        *bytes = s.buf_bytes;
    }
    return true;
}

void ui_get_geometry(uint16_t *width, uint16_t *height)
{
    if (width != NULL) {
        *width = s.w;
    }
    if (height != NULL) {
        *height = s.h;
    }
}

void ui_update_stats(const ui_stats_t *snap)
{
    if (snap == NULL) {
        return;
    }
    s.snap = *snap;
    s.has_snap = true;
}

esp_err_t ui_post_cmd(const char *utf8, size_t len)
{
    char buf[UI_CMD_LEN];

    if (!s.ready || s.cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (utf8 == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, utf8, n);
    buf[n] = '\0';
    if (xQueueSend(s.cmd_q, buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "UI 命令队列满，丢弃：%s", buf);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ============================ 命令解析 ============================ */

static char *trim_ws(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return p;
}

static void ui_set_page(bool stats)
{
    if (stats) {
        lv_obj_add_flag(s.page_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.page_stats, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.page_stats, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.page_main, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_apply_one(char *kv)
{
    char *eq = strchr(kv, '=');
    if (eq == NULL) {
        ESP_LOGW(TAG, "UI 命令缺 '='：'%s'", kv);
        return;
    }
    *eq = '\0';
    char *key = trim_ws(kv);
    char *val = trim_ws(eq + 1);
    if (*key == '\0') {
        return;
    }

    if (strcmp(key, "title") == 0) {
        lv_label_set_text(s.lbl_title, *val ? val : "MeidaP4");
    } else if (strcmp(key, "text") == 0) {
        lv_label_set_text(s.lbl_dyn, val);
    } else if (strcmp(key, "theme") == 0) {
        bool dark = !(strcmp(val, "light") == 0);
        s.theme_dark = dark;
        s.pal = dark ? PAL_DARK : PAL_LIGHT;
        ui_apply_theme();
    } else if (strcmp(key, "page") == 0) {
        ui_set_page(strcmp(val, "stats") == 0);
    } else if (strcmp(key, "overlay") == 0) {
        bool on = !(strcmp(val, "0") == 0);
        if (on) {
            lv_obj_remove_flag(s.lbl_fast, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s.lbl_slow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s.lbl_seq, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s.lbl_fast, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s.lbl_slow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s.lbl_seq, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        ESP_LOGW(TAG, "未知 UI 命令键 '%s'（支持 title/theme/page/text/overlay）", key);
    }
}

void ui_apply_cmds(void)
{
    char buf[UI_CMD_LEN];

    if (!s.ready || s.cmd_q == NULL) {
        return;
    }
    while (xQueueReceive(s.cmd_q, buf, 0) == pdTRUE) {
        char *p = buf;
        while (p != NULL && *p != '\0') {
            char *sep = strpbrk(p, ";&,");
            if (sep != NULL) {
                *sep = '\0';
                sep++;
            }
            ui_apply_one(p);
            p = sep;
        }
    }
}
