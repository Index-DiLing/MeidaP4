# MeidaP4 · I0 线协议（设备侧）
#
#   wu_codec.c      —— **纯 C** 编解码器（只 include <stdint.h>/<stddef.h>/<string.h>）。
#                      它同时被原生 gcc 编译（test/host/test_codec.c），用来和 web/proto.js
#                      做逐字节对照。**不要**往那个文件里加任何 IDF 依赖
#                      （见 wu_codec.h 顶部注释与 docs/protocol.md §8）。
#   webusb_proto.c  —— IDF 侧服务（窗口 W=8 / ACK / 200 ms 重传 ×5 / HELLO / 3 s link_down
#                      / CTRL 各 opcode / LOG / TELEM）。数据面全部经 usb_composite。
# ⚠️ 2026-10 修复（Lead 介入）：`webusb_proto.c` 一度被列进 SRCS 但文件还不存在，
#    导致 ninja 在**编译任何东西之前**直接失败：
#      ninja: error: '.../webusb_proto.c', needed by '...webusb_proto.c.obj',
#                  missing and no known rule to make it
#    因为 main/CMakeLists.txt 会动态 require 所有存在的组件，这个组件一进构建图，
#    CMake 就在 configure 期登记了那个源文件 → **全队的 `-Mode prod` 构建全挂**。
#
#    所以 SRCS 一律用 EXISTS 守卫：文件就绪就编，没就绪就跳过，绝不让"半落地"变成全队阻塞。
#    （加新源文件时请照此模式，不要直接往 SRCS 里裸写。）
set(meidap4_srcs "wu_codec.c")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/webusb_proto.c")
    list(APPEND meidap4_srcs "webusb_proto.c")
endif()

# I2（video_pipe）是**强依赖**：TELEM 要读它的统计、UI_CMD 要投递给它。
# Lead 2026-10 明确要求不要用 weak 引用容忍"组件未落地"（probe-report §3.2 的弱符号陷阱），
# 所以这里直接 REQUIRES；video_pipe 缺失就让构建**响亮地**失败，而不是静默降级。
idf_component_register(
    SRCS
        ${meidap4_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        usb_composite
        video_pipe
    PRIV_REQUIRES
        esp_timer
)
