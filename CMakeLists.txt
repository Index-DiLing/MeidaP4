# The following boilerplate has to be in this project's CMakeLists in this exact order for cmake to
# work correctly.
cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# ⚠️ 不要试图用 IDF_COMPONENT_MANAGER=0 来规避共享的 dependencies.lock。
#
# 2026-10 实测结论（WS-webusb 发现 + Lead 复核 IDF 源码）：
#   * IDF v6.1 只有在组件管理器**启用**时才注册 `<project>/managed_components/`
#     （tools/cmake/build.cmake:653-724 的 IDF_COMPONENT_MANAGER=1 分支）。
#     管理器关闭时该目录**完全不参与构建**。
#   * 于是 `managed_components/espressif__esp_tinyusb` 的 CMakeLists.txt:78-84 会加一条
#     对**名字就叫 `tinyusb`** 的组件的依赖 —— 而注册表安装出来的目录名是
#     `espressif__tinyusb`，名字对不上 → configure 直接报
#     "Failed to resolve component 'tinyusb'"。
#   * 把 managed_components 塞进 EXTRA_COMPONENT_DIRS **也救不了**：组件名取自目录基名
#     （component.cmake:159 `set(component_name ${base_dir})`），仍然是带前缀的名字。
#
# 因此全项目统一**启用**组件管理器；并发冲突由 tools/build.ps1 的自动重试处理
# （组件管理器自带锁，撞上就等 20 s 重试）。并发纪律见
# docs/contracts/frozen-allocation.md §7。
# ---------------------------------------------------------------------------

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MeidaP4)
