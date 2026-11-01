# MeidaP4 · file_service —— FILE 通道设备侧服务（TF 卡 /sdcard 上的文件操作）
#
# 依赖方向（只向内）：
#   file_service → webusb_proto（协议服务层：注册 FILE handler / 回包）
#                → sd_storage  （挂载状态 + 唯一挂载点来源，本组件不写第二份 "/sdcard" 字面量）
#
# PRIV_REQUIRES 说明（公开头只暴露 esp_err_t/bool/uint32_t，故这些都不必外泄）：
#   esp_timer -> esp_timer_get_time()（心跳文件里的 uptime_ms）
#   fatfs     -> esp_vfs_fat_info()（心跳里的 sd_total_kb / sd_free_kb）
#
# 消费者：main/main_prod.cpp 调用 file_service_start()（在 usb_vendor_start() 之前）。
idf_component_register(
    SRCS
        "file_service.c"        # FILE 协议服务（handler + 请求队列 + 句柄表 + 各 op）
        "file_share_probe.c"    # PC↔P4 文件级共享探针（心跳 / PC2P4.TXT / README）
    INCLUDE_DIRS "include"
    REQUIRES webusb_proto sd_storage
    PRIV_REQUIRES esp_timer fatfs
)
