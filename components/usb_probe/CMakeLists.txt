idf_component_register(
    SRCS "usb_probe.c"
    INCLUDE_DIRS "include"
    # esp_tinyusb 是 IDF 组件管理器安装的托管组件，组件名为 espressif__<name>
    REQUIRES espressif__esp_tinyusb
    # esp_timer：心跳/空闲计时用 esp_timer_get_time()
    PRIV_REQUIRES esp_timer
)

# ---------------------------------------------------------------------------
# esp_tinyusb 2.3.0 的 MTP 打包缺陷（链接期规避，真机已定位根因）
#
# tud_mtp_* 的**强实现**在 espressif__esp_tinyusb 的 mtp/tinyusb_mtp_transport.c 里，
# 但整个工程没有任何目标文件引用那个编译单元，静态库成员不会被提取；
# 于是 espressif__tinyusb 里 mtp_device.c:62/70 的 TU_ATTR_WEAK 默认实现反而生效：
#   - 弱 tud_mtp_command_received_cb() 返回负值 → mtp_device.c:425 置 MTP_PHASE_ERROR
#     → mtp_device.c:512-516 stall EP 02 / EP 82（Windows 发 OpenSession 一上来就被 stall）
#   - 弱 tud_mtp_request_get_device_status_cb() 返回 <= 0 → mtp_device.c:367 stall EP0
#   主机侧表现为「MTP 设备枚举成功、驱动绑定成功，但 WPD 建不出设备节点」。
#
# --undefined=<sym> 让链接器把该符号当成必须解析的未定义符号（同时成为 --gc-sections 的根），
# 从而强制提取定义了它的归档成员；mtp_dispatch() 只由 tinyusb_mtp_transport.c 定义。
# 只在 MTP 打开时加：模式 A（CONFIG_TINYUSB_MTP_ENABLED=n）没有这个符号，加了会链接失败。
# 不要改 managed_components/ 下的上游文件（会被组件管理器覆盖）。
# ---------------------------------------------------------------------------
if(CONFIG_TINYUSB_MTP_ENABLED)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=mtp_dispatch")
endif()
