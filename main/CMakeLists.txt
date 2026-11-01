# MeidaP4 · main
#
# ⚠️ 为什么这里要**动态**列组件（2026-10 实测得到的教训）：
#   ESP-IDF **只构建从 main 出发可达的组件**。实测：`-Mode prod` 构建里 `usb_composite` /
#   `video_pipe` / `usb_net` / `webusb_proto` / `sd_storage` 的 compile_commands 条目数都是 **0**
#   —— 它们根本没被编译。也就是说，只要 main 不 require 某个组件，那个组件里的语法错误
#   永远不会在构建里出现，写入者会拿到**假绿**。所以完整版的每个组件都必须在这里被 require。
#
# ============================================================================
# ⚠️ 入口选择**只能**用环境变量 `MEIDAP4_MODE`（由 tools/build.ps1 设置）
# ============================================================================
# 为什么**既不能**用 `CONFIG_PROD_APP_ENABLE` 之类的 Kconfig 值、**也不能**用 `-D` 传入：
#   IDF 的 component manager 在"缺 Kconfig 值"时会**重跑一次 configure**。
#   * 用 Kconfig 值：第一遍 sdkconfig 还没生成 ⇒ CONFIG_* 在 CMake 里全空 ⇒ 走错分支，
#     且第一遍的注册**已经生效**。
#   * 用 `-D`：实测第一遍里 `-DMEIDAP4_MODE=prod` **不可见**（第一遍打印 MEIDAP4_MODE=a、
#     第二遍才是 prod），同样第一遍已生效。
#   实测症状一致：日志出现两条入口 STATUS，而 ninja 给 main_prod.cpp 的编译行
#   **却带着 usb_probe/mtp_probe 的 include** → `fatal error: mtp_backend.h: No such file`。
#   ⇒ 结论：**构建结构只能依赖"第一遍 configure 就可见"的输入**；环境变量被所有子进程继承，
#     是唯一可靠的通道。这是本工程"一个事实只能有一个来源"那条教训的又一个实例。

if(NOT DEFINED ENV{MEIDAP4_MODE})
    set(MEIDAP4_MODE "a")   # 兜底：当作探测固件处理；正常路径由 build.ps1 显式设置
else()
    set(MEIDAP4_MODE "$ENV{MEIDAP4_MODE}")
endif()

set(MEIDAP4_PROD_COMPONENTS
    usb_composite   # I1 复合层（Lead）
    video_pipe      # I2 帧源（WS-V：LVGL 离屏 + 硬件 JPEG；内部 REQUIRE ui）
    usb_net         # I3 网络桥接
    webusb_proto    # I0 线协议（设备侧）
    usb_vendor      # vendor 双功能
    sd_storage      # SD 卡
    mtp_backend     # MTP 卷 + 函数注册
    net_hosted      # esp_hosted STA
    file_service    # FILE 通道设备侧服务（WS-F：TF 卡共享）
)

if(MEIDAP4_MODE STREQUAL "prod")
    # 完整版：8 个组件**必须全部存在**，缺一个就 FATAL（不再是静默 NOTICE）
    set(MEIDAP4_MISSING "")
    foreach(_c ${MEIDAP4_PROD_COMPONENTS})
        if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/${_c}/CMakeLists.txt")
            list(APPEND MEIDAP4_MISSING ${_c})
        endif()
    endforeach()
    if(MEIDAP4_MISSING)
        message(FATAL_ERROR "[main] prod 构建缺少完整版组件：${MEIDAP4_MISSING}")
    endif()

    set(MEIDAP4_SRCS "main_prod.cpp")
    set(MEIDAP4_ENTRY_REQS ${MEIDAP4_PROD_COMPONENTS})
else()
    # 探测固件（模式 a/b）
    set(MEIDAP4_SRCS "main.cpp")
    set(MEIDAP4_ENTRY_REQS usb_probe mtp_probe)
endif()

# ⚠️ 两个入口**互斥**，不是"都编、运行时二选一"：
#   `components/usb_probe` 与 `components/usb_composite` **各自定义**了强符号
#     tud_descriptor_bos_cb()  /  tud_vendor_control_xfer_cb()
#   同时进链接就是 multiple definition；而且探测期的 usb_probe 自带另一套端点分配（模式 A/B），
#   与完整版冻结表不一致 —— 混进任何一个都是错误。
#   prod 时**不 require** usb_probe/mtp_probe ⇒ IDF 只构建可达组件 ⇒ 它们整个不参与。
foreach(_s ${MEIDAP4_SRCS})
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_s}")
        message(FATAL_ERROR "[main] 入口源文件不存在：${_s}")
    endif()
endforeach()

message(STATUS "[main] MEIDAP4_MODE=${MEIDAP4_MODE} SRCS=${MEIDAP4_SRCS} REQUIRES=${MEIDAP4_ENTRY_REQS}")

idf_component_register(SRCS ${MEIDAP4_SRCS}
                    INCLUDE_DIRS "."
                    REQUIRES ${MEIDAP4_ENTRY_REQS})
