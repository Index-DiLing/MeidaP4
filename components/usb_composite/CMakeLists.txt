# MeidaP4 · I1 USB 复合层
#
# usb_composite_desc.c 是**纯计算**（描述符组装 + 端点/FIFO 预算 + 自检），刻意不依赖
# ESP-IDF 运行时，这样 test/host/test_composite_desc.c 能用原生 gcc 编译它做离线校验。
# 不要往那个文件里加 IDF 调用 —— 会打断离线验证链（见 docs/contracts/frozen-allocation.md §7）。
idf_component_register(
    SRCS
        "usb_composite.c"
        "usb_composite_desc.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_tinyusb
)
