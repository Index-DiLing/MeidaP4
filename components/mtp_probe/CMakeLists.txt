idf_component_register(
    SRCS "mtp_probe.c"
    INCLUDE_DIRS "include"
    # mtp_probe.h 只暴露 bool/esp_err_t，故其余依赖都可以是 PRIV_REQUIRES。
    # esp_timer 是 ESP_LOG 时间戳/uptime 用的（IDF 6.1 会校验 include 是否在 requirements 里）。
    PRIV_REQUIRES espressif__esp_tinyusb fatfs vfs esp_partition wear_levelling esp_timer
)
