# MeidaP4 · I3 网络桥接（WS-net）
#
#   usb_net.c        = esp_netif(DHCP server) + NAPT + DNS 透传 + 数据面 + 统计
#   usb_net_ncm.c    = NCM 描述符片段 + 通过 I1（usb_composite）注册
#   usb_net_bridge.c = 仅桥接降级模式编译（要用 esp_private/wifi.h 的 esp_wifi_internal_*）
set(usb_net_srcs "usb_net.c" "usb_net_ncm.c")
if(CONFIG_APP_USBNET_MODE_BRIDGE)
    list(APPEND usb_net_srcs "usb_net_bridge.c")
endif()

idf_component_register(
    SRCS
        ${usb_net_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_tinyusb
        usb_composite
        esp_netif
    PRIV_REQUIRES
        esp_event
        lwip
        esp_wifi
)

# ---------------------------------------------------------------------------
# esp_tinyusb 2.3.0 打包缺陷（**第二实例**，与 MTP 那处完全同构，见 probe-report §3.2）
#
# 上游 ncm_device.c:694 无条件引用 `tud_network_recv_cb`，其**强定义**在
# espressif__esp_tinyusb/tinyusb_net.c:165 —— 与 `tinyusb_net_init`（:121）**同一个编译单元**。
# 若工程里没有任何目标文件引用该归档成员，静态库成员不会被提取 →
#   undefined reference to `tud_network_recv_cb`（实测，build-net-prod / build-verify-prod）
# --undefined=<sym> 让链接器把它当成必须解析的未定义符号（同时成为 --gc-sections 的根），
# 从而强制提取定义了 `tinyusb_net_init` 的那个成员。
#
# 必须用 CONFIG 守卫：NCM=n 时 tinyusb_net.c **不参与编译**，强行引用会把 Mode a/b 的链接搞坏。
# 不要改 managed_components/ 下的上游文件（会被组件管理器覆盖）。
# ---------------------------------------------------------------------------
if(CONFIG_TINYUSB_NET_MODE_NCM OR CONFIG_TINYUSB_NET_MODE_ECM_RNDIS)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=tinyusb_net_init")
endif()
