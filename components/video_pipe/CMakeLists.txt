# MeidaP4 · I2 帧源（视频管线）
#
# 阶段一 MJPEG：LVGL 离屏渲染（components/ui） → 硬件 JPEG（IDF 自带 esp_driver_jpeg）
#                → 三缓冲池 → `video_pipe_get_frame()`。
# 阶段二 H.264：同一接口，只换 fmt 与编码器（espressif/esp_h264 已在 main/idf_component.yml 里声明）。
#
# ⚠️ `ui` 必须在这里 REQUIRES：IDF 只构建"从 main 可达"的组件，而 main 的 REQUIRES 里
#    只有 video_pipe —— ui 靠这条边进入构建。构建日志里看不到 `components/ui/ui.c` 的编译
#    条目就说明这条边断了（那是假绿）。
idf_component_register(
    SRCS
        "video_pipe.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ui
        esp_driver_jpeg
        esp_timer
)
