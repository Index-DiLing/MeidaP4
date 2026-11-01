# MeidaP4 · WS-webusb 的两个 vendor 功能（IF1 视频 / IF2 控制）
#
# 依赖：usb_composite（I1 装配器，冻结头）+ webusb_proto（I0 设备侧服务）
#       + **video_pipe（I2 帧源，强依赖）**。
#
# ⚠️ 2026-10 Lead 裁决：**不要**用 weak 引用 / __has_include 兜底来容忍"I2 未落地"。
#    probe-report §3.2 的教训：esp_tinyusb 的 MTP 强符号因无人引用而没被链接、弱默认胜出，
#    结果所有 MTP 命令 stall —— 弱符号会把"某组件根本没进构建图"这类**真实缺陷**
#    掩盖成"暂时没有帧"。`components/video_pipe` 现在有 CMakeLists.txt + 实现（Lead 的占位版），
#    所以这里直接 REQUIRES，缺了就**响亮地**失败。
set(uv_reqs usb_composite webusb_proto video_pipe)

# ⚠️ 2026-10 修复（Lead 介入，全队阻塞 —— 与 components/webusb_proto 那次同源）：
#    SRCS 里裸列了 `usb_vendor_video.c` / `usb_vendor_ctrl.c`，但当时**两个文件都不存在**，
#    于是 CMake **generate 阶段**硬失败（这一步按文件守卫救不了，因为 CMakeLists.txt 自己存在、
#    main 就会 require 这个组件）：
#      CMake Error at component.cmake:494 (add_library): Cannot find source file: .../usb_vendor_video.c
#    ⇒ 任何人的 `-Mode prod` 构建都停在 configure。
#
#    **规则：SRCS 只能列"此刻确实存在"的文件。** 加新源文件时照下面这样写，
#    不要让"先登记、后落地"变成全队阻塞。
set(uv_srcs "usb_vendor.c")
foreach(_f usb_vendor_video.c usb_vendor_ctrl.c)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_f}")
        list(APPEND uv_srcs "${_f}")
    endif()
endforeach()

idf_component_register(
    SRCS
        ${uv_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${uv_reqs}
)
