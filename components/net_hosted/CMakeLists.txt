# MeidaP4 · WS-net 上游侧（esp_hosted STA）
#
# net_hosted.h 里 include 了 esp_netif.h → esp_netif 必须是 **public** REQUIRES。
# esp_hosted 的公开头（esp_hosted.h，取 C6 握手版本用）在它自己的 host/compat/include 里，
# 用**目录名** espressif__esp_hosted 引用（托管组件的组件名 = 目录基名，见
# tools/cmake/component.cmake:159 `set(component_name ${base_dir})`；usb_probe 用的也是这个名字）。
idf_component_register(
    SRCS
        "net_hosted.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_netif
    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_timer
        nvs_flash
        espressif__esp_hosted
)
