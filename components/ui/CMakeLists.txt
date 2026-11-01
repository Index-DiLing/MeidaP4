# MeidaP4 · UI（LVGL 9.2 离屏显示 + 仪表盘）
#
# 依赖方向（单向，不允许反向）：
#     video_pipe  →  ui  →  { lvgl, sd_storage }
#
# ui **不依赖** video_pipe：它只通过 `ui_update_stats()` 接收一份纯数值快照，
# 这样 video_pipe 可以随时替换实现，UI 侧不用改。
#
# sd_storage：仪表盘要显示 TF 卡容量。**不能**用 statvfs —— ESP-IDF 的 FATFS VFS
# 没实现 statvfs（恒 ENOSYS），真机会误报 "not mounted"。容量走 sd_storage 的封装，
# 挂载点也只有 sd_storage 一处定义（不在这里第二份硬编码）。
idf_component_register(
    SRCS
        "ui.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl
        sd_storage
        esp_timer
)
