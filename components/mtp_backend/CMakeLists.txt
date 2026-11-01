# MeidaP4 · MTP 后端（WS-storage）
#
# 依赖：
#   espressif__esp_tinyusb -> tinyusb_mtp.h（MTP 类驱动 glue）+ tusb.h（TUSB_* 常量）
#   usb_composite          -> usb_composite.h（I1 契约；PRIV 即可，公开头不暴露它）
#   sd_storage             -> sd_storage.h（挂载点/容量）
idf_component_register(
    SRCS "mtp_backend.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        espressif__esp_tinyusb
        usb_composite
        sd_storage
)

# ---------------------------------------------------------------------------
# ⚠️ 必须：-Wl,--undefined=mtp_dispatch —— esp_tinyusb 2.3.0 的 MTP 打包缺陷
#
# 真机定位（probe-report §3.2）：tud_mtp_* 的**强实现**在 esp_tinyusb 的
# mtp/tinyusb_mtp_transport.c 里，但那个编译单元没有任何东西引用 → 静态库成员不被提取；
# 于是 espressif__tinyusb 的 mtp_device.c:62/70 `TU_ATTR_WEAK` 默认实现反而生效：
#   * 弱 tud_mtp_command_received_cb() 返回负值 → mtp_device.c:425 置 MTP_PHASE_ERROR
#     → mtp_device.c:512-516 stall EP 02 / EP 82（Windows 一发 OpenSession 就被 stall）；
#   * 弱 tud_mtp_request_get_device_status_cb() 返回 <= 0 → mtp_device.c:367 stall EP0。
# 主机侧现象是「MTP 设备枚举成功、wpdmtp.sys 绑定 problem=0，但 WPD 建不出设备节点、
# 资源管理器里不出现便携设备」——没有设备侧日志的话极难定位。
#
# `--undefined=<sym>` 让链接器把该符号当成必须解析的未定义符号（同时成为 --gc-sections 的根），
# 从而强制提取定义了它的归档成员；`mtp_dispatch()` 只由 tinyusb_mtp_transport.c 定义。
# 探测期加了这个参数后 nm 实测四个 tud_mtp_* 回调全部从 W 变 T。
#
# 用 CONFIG_TINYUSB_MTP_ENABLED 守卫：模式 A/B 里 MTP=n 时没有这个符号，加了会链接失败。
# 不要改 managed_components/ 下的上游文件（会被组件管理器覆盖）。
# ---------------------------------------------------------------------------
if(CONFIG_TINYUSB_MTP_ENABLED)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=mtp_dispatch")
endif()
