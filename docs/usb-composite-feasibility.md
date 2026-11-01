# ESP32-P4 单 USB 口「四合一」方案可行性评估

> 评估对象：ESP32-P4 + ESP32-C6 开发板，通过板载 USB 口（USB 2.0 高速 OTG，封装管脚 49~50）
> 作为**复合 USB 从设备**同时提供：① 移动存储（代理 SD 卡）② CDC 数据通道 ③ UVC 摄像头（输出 LVGL 画面）④ 共享 Wi-Fi（经 C6）
>
> 评估依据：ESP-IDF v6.1 源码（`C:\esp\v6.1\esp-idf`）、ESP 组件仓库缓存源码（esp_tinyusb 2.3.0 / tinyusb 0.21.0~2 / esp_hosted 3.0.7 / esp_wifi_remote 1.6.4）、Espressif 官方文档。
> 结论中的每一条技术约束都标注了出处。

---

## 0. 结论摘要

| 目标功能 | 单独可行性 | 关键依据 | 主要风险 |
|---|---|---|---|
| ① 移动存储 MSC 代理 SD 卡 | ✅ 现成 | `tinyusb_msc_new_storage_sdmmc()` / blockdev API 官方提供 | SD 槽位与 C6 的 SDIO 抢 SDMMC 槽；PC 占用时 P4 不能挂载 FATFS |
| ② CDC 数据通道 | ✅ 现成 | `CONFIG_TINYUSB_CDC_ENABLED`；Windows 10+ 自带 `usbser.sys` | 无 |
| ③ UVC 摄像头输出 LVGL 画面 | ⚠️ **硬骨头** | esp_tinyusb **没有** UVC 设备类；但上游 tinyusb 带 `video_device.c`，DCD 支持等时端点 | 需自行移植 UVC 类 + 自定义复合描述符；**未压缩视频帧率有硬上限** |
| ④ 共享 Wi-Fi（USB 网卡 + C6） | ⚠️ 链路通、需自写应用 | esp_hosted 3.0.7 + IDF 内置 `esp_wifi/remote`；esp_tinyusb 支持 NCM/RNDIS | IDF 两个官方 tether 示例都**硬排除** P4；C6 固件版本必须匹配 |
| 四者**同时**挂一个口 | ⚠️ 极限可行 | IN 端点预算 7+EP0 = 8/8，**零余量** | 4 KB TX FIFO 分摊、等时与 bulk 争抢、供电 |

**一句话结论**：技术上**可行**，但不是"拼现成轮子"，而是一个**中等偏上的系统工程**。四个功能里 ①② 是现成的，④ 是有路但要自己写应用，③ 需要**动 USB 协议栈源码**（移植 UVC 设备类），而"四合一同时在线"受**端点数量零余量**约束。建议分 4 个阶段推进，先拿下 ①②，再攻 ④，最后做 ③。

> **1080p@30fps 专项结论见第 12 节**：未压缩**物理不可能**（1920×1080×2 B×30 = 124 MB/s，是 USB 2.0 HS 线速 60 MB/s 的 2.07 倍）；**MJPEG 有条件可行**——P4 硬件 JPEG 编码器实测 1080p 达 40 fps，USB 等时路径每帧预算 273 KB，而 LVGL 这种大面积纯色的 UI 画面 JPEG 体积天然远低于此。真正瓶颈是 LVGL 全屏重绘与 PSRAM 带宽，不是 USB。

---

## 1. 前提澄清：你说的 USB 口是哪一个

你引用的 Datasheet 原文是：

> USB 2.0 高速 OTG 接口的 USB2 OTG PHY DM (USB_D−) 和 DP (USB_D+) 使用专用数字管脚，**管脚序号为 49~50**。

这里的 49~50 是**封装管脚序号（pin number）**，不是 GPIO 编号。这点很关键，顺带纠正一处容易踩坑的地方：

- **ESP32-P4 的 GPIO49 / GPIO50 本身没有任何 USB 功能**。查 `components/soc/esp32p4/register/hw_ver{1,3}/soc/io_mux_reg.h`，这两个 IO MUX 只有三个功能：`GPIO49/50`、`EMAC_PHY_TXEN` / `EMAC_RMII_CLK`、`DBG_FLASH_CS` / `DBG_FLASH_Q`。
- 你的口是**专用复用引脚** → 也就是 **USB 2.0 高速 OTG（480 Mbps）**，这是最好的情况。

P4 一共有两个可用作 USB 从设备的口（源码 `components/esp_hal_usb/include/soc/usb_periph.h`、`io_mux_reg.h`）：

| 口 | 类型 | D− / D+ | 能力 | TX FIFO |
|---|---|---|---|---|
| **HS**（你的口） | USB 2.0 高速，内置 UTMI PHY | 专用管脚（封装 49/50） | EP0 + 15 个端点，**IN 端点最多 8 个（含 EP0）** | 4 KB |
| FS | USB 1.1 全速，内置 FSLS PHY | **GPIO26 (D−) / GPIO27 (D+)** | EP0 + 6 个端点，**IN 端点最多 5 个（含 EP0）** | 1 KB |

另外 USB-Serial-JTAG 是独立外设，在 GPIO24/25，只能做串口，不做通用 USB 从设备。

> 出处：Espressif [ESP-USB Programming Guide — USB Device Stack (esp32p4)](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_device.html)；源码 `tinyusb 0.21.0~2/src/portable/synopsys/dwc2/dwc2_esp32.h:78-81`。

**待确认**：你的板子是否把 FS 口（GPIO26/27）也引出来了？这会影响第 7 节的备选架构。

---

## 2. ESP32-P4 相关硬件能力盘点（决定方案上限）

| 能力 | 结论 | 出处 |
|---|---|---|
| 等时（ISO）端点 | ✅ **DCD 完整支持**：`dcd_edpt_iso_alloc` / `dcd_edpt_iso_activate`、ISO DPID 翻转、`dcd_sof_enable`（注释明确点名 audio/video 等时类）、不完整 ISO-IN 恢复 | `dcd_dwc2.c:666, 671, 324-327, 429-437, 611-624, 1178-1210` |
| **高带宽等时（每微帧多事务）** | ❌ **不支持**。`tu_edpt_packet_size()` 实现是 `wMaxPacketSize & 0x7FF`，高带宽的"附加事务数"位被直接丢弃，DWC2 `MPSIZ` 只写入 ≤1024 | `src/common/tusb_types.h`（函数体）、`dcd_dwc2.c:311, 322` |
| PPA 像素加速器 | ✅ 支持 **RGB565 → YUV422（YUYV/UYVY/YVYU/VYUY）** 硬件色彩空间转换 | `components/esp_driver_ppa/include/driver/ppa.h`、`esp_hal_ppa/.../ppa_ll.h:264-385` |
| 硬件 JPEG 编码器 | ✅ `SOC_JPEG_ENCODE_SUPPORTED = 1`，组件 `espressif/esp_jpeg` | `components/soc/esp32p4/include/soc/soc_caps.h` |
| 硬件 H.264 编码器 | ✅ `SOC_H264_ENCODER_SUPPORTED = 1`（但 UVC 不支持 H.264，只能走自定义类/网络流） | 同上 |
| MIPI-CSI + ISP | ✅ 支持（可用于"真实摄像头画面 + LVGL 叠 UI"的后续扩展） | 同上 |
| CPU / 内存 | 2× RISCV @400 MHz（当前 sdkconfig），L2 128 KB | `sdkconfig` |

**最关键的一条**：等时端点能做，但**一个等时 IN 端点的吞吐上限 = 1024 B/微帧 × 8000 微帧/s = 8.192 MB/s**。这一条直接决定了第 4 节的视频帧率，也决定了"未压缩 YUY2 只能当低帧率方案"。

---

## 3. 端点预算 —— 决定"四合一"能不能塞进一个口

USB 设备的端点是**共享硬件资源**，不是软件问题。四个功能各需：

| 功能 | IN 端点 | OUT 端点 |
|---|---|---|
| ① MSC | 1 × Bulk IN | 1 × Bulk OUT |
| ② CDC-ACM（单路） | 1 × Interrupt IN（状态通知）+ 1 × Bulk IN | 1 × Bulk OUT |
| ③ UVC | 1 × **Isochronous** IN（视频流）+ 1 × Interrupt IN（VC 状态） | 0（可选的静态图 Bulk 省略） |
| ④ USB 网卡（NCM/RNDIS） | 1 × Interrupt IN（通知）+ 1 × Bulk IN | 1 × Bulk OUT |
| **合计** | **7 个 IN + EP0 = 8** | **3 个 OUT** |

- HS 口上限：**IN 端点 8 个（含 EP0）** → 7 + EP0 = 8，**刚好卡满，余量为 0**。
- 这意味着**任何**额外 IN 端点都会枚举失败：UVC 多一个静态图端点、第二路 CDC、以后想加 UAC 音频、都放不下。
- OUT 端点（3 个）很宽松。
- 另一个隐性约束：**HS 口 TX FIFO 总共只有 4 KB**，要在 7 个 IN 端点间分配（`dfifo_alloc`）。等时端点要 1024 B，MSC/NCM 各想要 ≥512 B 双缓冲 → 会明显压缩 MSC/NCM 的突发吞吐。
- **如果这个口是 FS 口就完全做不了**：FS 只有 5 个 IN 端点上限，连 7 都不到。这也是为什么第 1 节的确认很重要。

> 强制点：`dcd_dwc2.c:242` `TU_ASSERT(epnum < ep_count)`；`:256-259` `TU_ASSERT(_dcd_data.allocated_epin_count < dwc2_controller->ep_in_count)`。

---

## 4. 带宽预算

### 4.1 视频（唯一真正的带宽敏感项）
| 格式 | 单帧大小(640×480) | 结论 |
|---|---|---|
| YUY2 未压缩 | 614,400 B | 受等时上限 8.192 MB/s → **≈13.3 fps 上限**（高带宽等时不支持，无法突破） |
| YUY2 未压缩 320×240 | 153,600 B | 上限 ≈53 fps → **30 fps 无压力** |
| MJPEG（硬件 JPEG） | 约 20~30 KB | 640×480@30 ≈ 0.6~1 MB/s → **轻松**，但需 UVC 声明 MJPEG 格式 |

### 4.2 其余功能
- MSC：受 SD 卡与 SDMMC 限制，USB 侧 HS bulk 足够；实际读吞吐预计 10~25 MB/s 量级。
- USB 网卡：C6 侧 Wi-Fi（经 SDIO）实际几十 Mbps，远低于 HS 上限。
- CDC：可忽略。
- **结论：HS 口带宽不是瓶颈（除未压缩视频外），瓶颈在端点数量 + FIFO + CPU/PSRAM。**

---

## 5. 逐项可行性拆解

### 5.1 ① 移动存储（代理 SD 卡）✅
- 官方 API 现成：`tinyusb_msc_new_storage_sdmmc(&config, &hdl)`；IDF ≥ 6.0.4 还可走通用块设备接口 `sdmmc_get_blockdev()` → `tinyusb_msc_new_storage_blockdev()`（IDF 6.1 具备）。
- **必须设计好的语义问题**：MSC 是把 SD 卡**块级裸设备**交给 PC，此时 P4 **不能**同时挂载 FATFS，否则必然损坏数据。两个选项：
  - **MSC（推荐先用）**：PC 拿完整块设备（能看到盘符、格式化、文件系统随便用），代价是 P4 侧要让出访问权（可用"弹出/插入"媒体切换机制做时间片）。
  - **MTP**：esp_tinyusb 也支持（`CONFIG_TINYUSB_MTP_ENABLED`），文件级共享、P4 可同时访问；代价是 Windows 端 MTP 体验较差（不是盘符，要装/等便携设备驱动）。
- ⚠️ **SDMMC 槽位冲突**：ESP-Hosted 用 SDIO 连 C6，默认占用 **SDMMC Slot 1**（CMD19 / CLK18 / D0~D3 = 14/15/16/17，复位 54）。SD 卡要用**另一个槽**（Slot 0）。ESP-Hosted 恰好有一个官方示例 `examples/mcu_hosted_sdio_sdmmc_combined` 就是解决这个共存的，直接参考。

### 5.2 ② CDC 数据通道 ✅
- `CONFIG_TINYUSB_CDC_ENABLED`，`tinyusb_cdcacm_init()`；还支持把 stdout 重定向到 CDC（`tinyusb_console_init()`）——对调试很有用（因为 HS 口不占串口）。
- Windows 10+ 内置 `usbser.sys`，一般免驱。

### 5.3 ③ UVC 摄像头（输出 LVGL 画面）⚠️ 最高风险

**事实（已 grep 确认）**：
- **esp_tinyusb 2.3.0 完全没有 UVC/Video 设备类**：搜 `CFG_TUD_VIDEO|CONFIG_TINYUSB_VIDEO|TUD_VIDEO|UVC|video_device` → 无任何匹配；`include/tusb_config.h` 里既没有 `CFG_TUD_VIDEO` 也没有 `CFG_TUD_AUDIO`。它只提供 CDC / MSC / MTP / HID / MIDI / Vendor / BTH / DFU / NET。
- 但它依赖的**上游 `espressif/tinyusb` 0.21.0~2 里带着 video 类源码**：`src/class/video/video_device.c`（1527 行）+ `video_device.h`，`CMakeLists.txt:28` 有编译，函数体由 `#if (CFG_TUD_ENABLED && CFG_TUD_VIDEO && CFG_TUD_VIDEO_STREAMING)` 守卫，默认 `CFG_TUD_VIDEO = 0`（`tusb_option.h:667-668`），**没有被裁剪或打补丁**。
- DCD 层等时端点支持完整（见第 2 节）。
- **另一个重要利好：上游 TinyUSB 的 UVC 类同时支持 Bulk 端点** —— `video_device.c:858` 注释 "Open bulk or isochronous endpoints"，`:864` 判等时，`:873` 明确 `TU_VERIFY(TUSB_XFER_BULK == ...)` 后走 `usbd_edpt_open`。把视频流端点声明为 Bulk，带宽上限可从等时的 8.192 MB/s 提到 ~35 MB/s（详见第 12.4 节）。
- **所以 UVC 是一个"软件栈缺口"，不是"控制器不可能"。**

**落地路线（按推荐度排序）**：
- **路线 A（推荐）**：把 `esp_tinyusb` vendor 成项目内本地组件（`components/esp_tinyusb/` + 在 `main/idf_component.yml` 用 `override_path` 覆盖注册表版本），然后：加 `CONFIG_TINYUSB_VIDEO_*` Kconfig → 在 `include/tusb_config.h` 里映射 `CFG_TUD_VIDEO=1` / `CFG_TUD_VIDEO_STREAMING=1` → **自写完整复合配置描述符**（含 UVC 的 IAD + VC 接口 + VS 接口 + 各 Alternate Setting）。机制上完全可行：`tinyusb.h` 的 `tinyusb_desc_config_t` 直接暴露 `full_speed_config` / `high_speed_config` 为裸 `uint8_t*`，用户可以定义整个复合描述符（缺省描述符只覆盖 CDC/MSC/MTP/NCM，其他类必须自带，见 `descriptors_control.c:205-233`）。官方 `esp_tinyusb` 的 `test_apps/usb_cv` 就是这个套路的示范（MSC + 手写 HID 复合描述符）。
- **路线 B**：绕开 esp_tinyusb、单用 TinyUSB（参考 `espressif/usb_device_uac` 1.3.1 的做法：自带 `tusb_config.h` + `usb_descriptors.c`，自己调 `tusb_init()`）。代价：丢失 esp_tinyusb 的 MSC/CDC/NCM 便利 API，且**两者不能共存**（两套 `tusb_config.h`、tinyusb 版本冲突 ^0.19.0~3 vs esp_tinyusb 的 0.21.0~2）。
- **路线 C**：放弃 UVC，改成 Vendor 自定义类 + 自写 PC 端程序。达不到"电脑端打开摄像头 App 就看到 LVGL 界面"的原始目标。

**视频数据通路（可行且高效）**：
```
LVGL 渲染 → PSRAM 帧缓冲(RGB565)
   → PPA 硬件转换 RGB565→YUYV → UVC 未压缩帧   (低分辨率/低帧率)
   → 或 硬件 JPEG 编码 → MJPEG → UVC MJPEG 帧   (推荐，640×480@30 轻松)
```
- 由于你没有 MIPI-DSI 屏，LVGL 用 **buffered display + 空 flush 回调**（full refresh 模式）渲染到 PSRAM 缓冲即可；避免 partial refresh，否则要做脏矩形合成。
- **PSRAM 是硬需求**：640×480 RGB565 单帧 600 KB，双缓冲 + YUY2 输出缓冲 ≈ 2~3 MB。**当前 sdkconfig 里 `CONFIG_SPIRAM` 根本没开**（见第 9 节）。
- PPA 的输入/输出缓冲需要按 **L1/L2 cache line（64 B）对齐**（`ppa.h:142` 明确要求）。
- 需要实测验证的点：`CONFIG_TINYUSB_MODE_DMA` 下等时端点的行为；Windows 对复合设备内 UVC（IAD）的枚举。

### 5.4 ④ 共享 Wi-Fi（C6 做射频 + USB 网卡给 PC）⚠️ 需自写应用

- **组件版本**：必须用 **esp_hosted 3.0.7**（≥3.0.3 才修了 IDF 6.1 的致命链接问题："IDF 6.1+: skip PRIVATE-linking the source-less esp_wifi_remote shim (was a fatal configure error)"）+ `esp_wifi_remote` 1.6.4。注意：**在 IDF 6.1 上 esp_wifi_remote 1.6.4 是个空壳**（`CMakeLists.txt:1-4` 检测到 `$IDF_PATH/components/esp_wifi/remote/Kconfig.wifi.in` 就直接 `return()`），真正的逻辑是 IDF 内置的 `components/esp_wifi/remote/` + esp_hosted 的 `host/wifi_remote_glue`。
- **必设配置**（照抄 esp_hosted 官方 P4 示例 `examples/wifi/apsta/mcu_host/sdkconfig.defaults:7-12`）：
  `CONFIG_ESP_HOSTED=y`、`ESP_HOSTED_HOST_FEAT_RPC=y`、`_RPC_EXT_V2=y`、`_SYSTEM=y`、`_WIFI=y`，外加 `ESP_WIFI_REMOTE_ENABLED=y`、`ESP_WIFI_REMOTE_LIBRARY_HOSTED=y`、`SLAVE_IDF_TARGET_ESP32C6=y`。**少了 FEAT_WIFI 会静默退回 weak 桩，`esp_wifi_init()` 直接返回 `ESP_ERR_NOT_SUPPORTED`。**
- **传输**：P4+C6 默认 **SDIO**；P4 侧引脚 CMD19 / CLK18 / D0~D3 14/15/16/17 / slave-reset 54（可改），**C6 侧 SDIO 引脚固定不可改**。备选 SPI-HD / UART。
- ⚠️ **你的" C6 已烧录固件"必须核对版本**：所有缓存组件里**没有任何预编译 `.bin`**，CP 固件是源码工程（3.x 用 `cp/`，`eh.py -p <CP_PORT> flash monitor` 烧写；2.x 用 `slave/`）。版本矩阵：3.x host ↔ 3.x CP 官方支持；3.x host + 1.x/2.x CP 标"兼容但建议升级 CP"；文档强调 master/slave 版本要一致。**结论：大概率要把 C6 重刷成与 host 匹配的固件。**
- **给 PC 共享上网的两条架构**：
  - **(a) L2 桥接**（`tusb_ncm` 示例的做法）：P4 当透明二层桥，PC 直接从上游 AP 拿 IP。已在 esp_hosted 里打通（`esp_wifi_remote_net.c:68-95` + `wifi_remote_glue.c:102-116` 实现 `esp_wifi_internal_tx` / `_reg_rxcb`）。最简单。
  - **(b) NAPT 路由**（推荐）：PC 在 P4 后面的私网，P4 做 DHCP 服务器 + NAT + DNS 透传。需 `CONFIG_LWIP_IP_FORWARD=y` + `CONFIG_LWIP_IPV4_NAPT=y`（`components/lwip/Kconfig:259-270`）。**当前工程 `CONFIG_LWIP_IP_FORWARD` 是关闭的**。
  - 注意：**esp_tinyusb 自身没有 esp_netif 集成**（grep 无匹配），USB 网卡的 netif/DHCP server/收发桥接必须自己写——直接照抄 `examples/network/sta2eth/main/usb_ncm_iface.c:48-58, 113-181`（`tinyusb_net_init()` + `esp_netif`（`ESP_NETIF_DHCP_SERVER|ESP_NETIF_FLAG_AUTOUP`）+ 静态发送回调），并像 `sta2eth_main.c:39-63` 那样设置 MAC（建议用 STA 的 MAC）。
- ⚠️ **别再照着 `sta2eth` 示例硬套**：它的 USB 有线接口有硬条件 `Kconfig.projbuild:73-75` `depends on IDF_TARGET_ESP32S2 || IDF_TARGET_ESP32S3`，README 也明说 USB-NCM 只支持 S2/S3；`tusb_ncm` 示例目标同样只有 S2/S3/S31。**P4 不在官方支持目标里** → 必须自写应用（但 esp_tinyusb 2.3.0 本身声明支持 esp32p4，且必需的二层钩子 esp_hosted 已实现，所以是"没人拼好"而不是"拼不出来"）。
- **Windows 驱动**：esp_tinyusb 的 `CONFIG_TINYUSB_NET_MODE` 在 NCM 和 ECM/RNDIS 间**二选一**。NCM 在 Windows 11 / 较新 Windows 10 有原生驱动（TinyUSB 描述符还带 MS OS 2.0 描述符用于自动装驱动）；**若你的 PC 是 Windows 10 且 NCM 不识别，就切 `ECM_RNDIS` 用 RNDIS**（这是 Windows 从 XP 起就带驱动的类）。

---

## 6. 「四功能同时在线」的额外风险

1. **IN 端点零余量**（7+EP0 = 8/8）——任何描述符笔误或额外端点都会导致**整个复合设备枚举失败**（不是单功能失效，是全部掉线）。
2. **4 KB HS TX FIFO 分摊**：等时端点要 1024 B，MSC/NCM 各想要 ≥512 B → 吞吐会被迫取舍。
3. **等时与 bulk 争抢**：视频流是等时的（带宽预留），MSC 大文件传输会与 NCM 抢 bulk 带宽。目前只有一个口、一台主机，没有第二条 USB 通路来分流。
4. **供电**：Wi-Fi 发射峰值 + PSRAM + SD 卡同时工作。如果板子由 PC 的 USB 总线供电（USB 2.0 上限 500 mA），很可能不够导致复位/枚举失败。**建议自供电**，并按官方文档接 VBUS 检测（电阻分压或比较器），用 `tinyusb_phy_config_t.self_powered` + `vbus_monitor_io`。
5. **CPU**：2×400 MHz 看着够，但 LVGL 软件渲染整屏 640×480 本身就是负担（复杂 UI 很难跑到 30 fps），再加上 Wi-Fi 协议栈、lwIP/NAT、USB 栈、SD 卡。**预计真实瓶颈是 LVGL 渲染 + PSRAM 带宽，而不是 USB。**
6. **HS 口必须显式选择**：`tinyusb_config_t.port` 要设成 `TINYUSB_PORT_HIGH_SPEED_0`（`include/tinyusb.h:25-33, 118`），否则默认走 FS 口。

---

## 7. 建议架构与备选

**主方案（单口四合一）**
- HS 口承载全部 4 个功能；视频用 **MJPEG（硬件 JPEG）**，避开等时带宽天花板。
- 若暂时不做 MJPEG，就把 UVC 降到 **320×240 YUY2 @30fps**（舒适区）。

**备选 A：双口拆分（如果板子引出了 FS 口 GPIO26/27）**
- HS 口：UVC + MSC（带宽大户）
- FS 口：CDC + USB 网卡（低速控制/网络）
- 优点：彻底绕开 IN 端点上限与等时争抢；缺点：PC 上出现**两个独立 USB 设备、需要两根线**（USB hub 只会让它们并列，不会合并成一个复合设备）。

**备选 B：三合一 + Wi-Fi 走 SoftAP**
- 丢掉 USB 网卡，C6 开 SoftAP（esp_hosted 的 `apsta` 示例确认 P4 host 支持 STA+AP 并发），PC 连 Wi-Fi 上网。省 2 个 IN 端点。

**备选 C：UVC 单独一口**
- 若四合一实测不稳定，最坏情况退化为"UVC 专用 + 其余功能轮流/另开一版固件"。

---

## 8. 分阶段实施路线图

| 阶段 | 内容 | 验收标准（必须可复现） |
|---|---|---|
| **P0 工程基建**（0.5~1 天） | 修复 IDF 环境变量；确定板级参数（flash 容量、PSRAM、分区表）；建 `main/idf_component.yml`；引入 esp_tinyusb/lvgl | `idf.py build` 通过；串口能打印；HS 口能被 PC 识别为一个 USB 设备 |
| **P1 组合①②**（1~2 天） | 照 `examples/peripherals/usb/device/tusb_composite_msc_serialdevice` 改：MSC 后端换成 SD 卡（注意 Slot 与 C6 的 SDIO 错开），加 CDC | PC 上**同时**出现盘符（能读写 SD）和 COM 口；SD 读写不损坏；P4 侧不挂 FATFS |
| **P2 组合④**（2~4 天） | esp_hosted 3.0.7 + 重刷匹配版本的 C6 固件；打通 STA 联网；再加 NCM/RNDIS + esp_netif DHCP server + NAPT | P4 能 ping 外网；**PC 通过 USB 网卡能上网**；测出实际吞吐；长跑不崩 |
| **P3 攻坚③**（3~7 天，最大不确定性） | 先做**独立最小 UVC**（静态 YUY2 测试图），验证 Windows 枚举成摄像头；再 vendor+改 esp_tinyusb 暴露 `CFG_TUD_VIDEO`；再引 LVGL→PPA→UVC；最后升级到 MJPEG | Windows「相机」App 能看到画面；帧率/延迟有实测数据；LVGL UI 正常刷新 |
| **P4 四合一定型**（2~5 天） | 手写完整复合描述符（CDC/UVC 的 IAD）、FIFO 调优、四路并发压测、供电与热稳定性 | 四功能**同时**在线且压测 1 小时无掉线；记录极限工况 |

**关键原则**：每个阶段都产出**可独立交付、可复现**的成果。特别是 P3，**一定先做"独立 UVC"再合并**——不要一上来就调四合一描述符，否则故障面太大。

---

## 9. 工程现状与必须先修的环境问题（本机实测）

| 项 | 实测结果 | 影响 |
|---|---|---|
| `IDF_PATH` 环境变量 | 原本是失效的 `E:\esp32\.espressif\v6.0.2\esp-idf`；**已由用户修正**。注意 DSH 会话进程会继承旧值，自动化脚本里需显式加载生效 | 修正前 `idf.py` 完全不可用 |
| `IDF_TOOLS_PATH` | 原本是失效的 `E:\esp32\.espressif`；**已修正** | 同上 |
| 真实 IDF | **`C:\esp\v6.1\esp-idf`**（`C:\esp` 是指向 `H:\.Import_CDiskLinker\esp` 的 Junction），版本 v6.1，工具链在 `C:\Espressif\tools` | `build/` 里的产物正是用 v6.1 构建的，匹配 |
| 环境激活的兜底方式 | `& "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"`（EIM 生成的 profile，会设好 IDF_PATH / IDF_TOOLS_PATH / python venv / PATH，并定义 `idf.py`） | 脚本化构建/CI 里用这个最稳 |
| `sdkconfig` | 基本是**通用模板默认值**：`FLASHSIZE=2MB`、`PARTITION_TABLE_SINGLE_APP`、**`CONFIG_SPIRAM` 完全没开**、CPU 400 MHz、`ESP32P4_REV_MIN_301` | 2 MB flash 装不下 LVGL+Wi-Fi+USB 应用；PSRAM 不开跑不了视频缓冲。**待按 hello_p4 重配** |
| **`E:\ESP_PROJECT\hello_p4`（可用参考）** | ✅ 已实测可用：**PSRAM 32 MB @ 250 MHz**（Hex 模式）、**Flash 32 MB**、**esp_hosted 3.0.7 + esp_wifi_remote 1.6.4**、SD 卡走 **SDMMC Slot 0（GPIO39~44 专用 IOMUX）4-bit / 40 MHz**、FATFS 已配好、另有 `README_CAMERA_TCP.md` 与 `main_sd_card.cpp.back`（含读写/格式化/擦除/基准测试） | **P0/P1/P2 直接复用其配置与代码**；`sdkconfig.defaults` 只 3 行（SPIRAM 相关），可直接继承 |
| C6 协处理器固件 | hello_p4 的 `dependencies.lock` 里 esp_hosted = 3.0.7 且工程可用 ⇒ **你板上的 C6 固件已经和 host 3.0.7 配对成功**，无需再纠结重刷（仍建议开机时读一次握手打印的版本号留档） | 原先"大概率要重刷"的担心解除 |
| `main/`（MeidaP4） | 只有 `CMakeLists.txt` + 空的 `main.cpp`，**没有 `idf_component.yml`**，没有任何依赖 | 组件全部要新增 |
| 组件仓库可达性 | ✅ 可达（`components.espressif.com` 正常响应）；esp_tinyusb 2.3.0、tinyusb 0.21.0~2、esp_hosted 3.0.7、esp_wifi_remote 1.6.4、esp_jpeg、usb_host_uvc、lvgl 等均可获取（多数已在本地缓存） | 无需担心拉包 |

---

## 10. 需要你确认/提供的信息

1. **开发板型号 + 原理图**（关键：SD 卡走线是 SDMMC 4-bit 还是 SPI？C6 的 SDIO 用了哪些脚？是否引出了 GPIO26/27 的 FS 口？USB 口是自供电还是总线供电？）
2. **C6 当前烧录的固件版本**（esp_hosted host/slave 版本必须匹配，很可能需要重刷）
3. **PC 端操作系统**（Windows 10 还是 11 —— 决定 USB 网卡用 NCM 还是 RNDIS）
4. **UVC 目标**：分辨率、帧率、以及 LVGL UI 的复杂度（决定选 YUY2 还是 MJPEG）
5. 板子**实际 flash 容量与 PSRAM 容量**（决定分区表与缓冲策略）

---

## 11. 参考出处

- [ESP-USB Programming Guide — USB Device Stack (esp32p4)](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_device.html)（P4 两个 USB 口的端点能力、MSC/NCM API、无 UVC 类）
- [esp_tinyusb 组件](https://components.espressif.com/components/espressif/esp_tinyusb)（targets 含 esp32p4）
- 本地源码：`C:\Espressif\tools\components\espressif\{esp_tinyusb\2.3.0, tinyusb\0.21.0~2, esp_hosted\3.0.7, esp_wifi_remote\1.6.4}`
- 本地 IDF：`C:\esp\v6.1\esp-idf`（`examples/peripherals/usb/device/tusb_composite_msc_serialdevice`、`tusb_ncm`、`examples/network/sta2eth`）

---

## 12. 专项：1080p@30fps 可行性

### 11.1 结论先行

| 路径 | 1080p@30fps | 依据 |
|---|---|---|
| 未压缩 YUY2 / RGB565 | ❌ **物理不可能** | 1920×1080×2 B×30 = **124.4 MB/s**，是 USB 2.0 HS 线速（480 Mbps = 60 MB/s）的 **2.07 倍**——连总线本身都装不下，与帧缓冲、CPU 无关 |
| **MJPEG（硬件 JPEG）** | ✅ **有条件可行** | 编码器实测 1080p 达 40 fps > 30 fps；USB 等时路径每帧预算 **273 KB**，UI 画面天然满足 |
| H.264（硬件编码器存在） | ⚠️ **不能走 UVC** | UVC 规范与 Windows 收件箱 UVC 驱动都不支持 H.264 载荷；只能自定义类 + 自写 PC 端，或走网络流 |

### 11.2 硬件 JPEG 编码器实测（决定性利好）

官方数据：`docs/en/api-reference/peripherals/jpeg.rst:326-397`（ESP32-P4，CPU 360 MHz / PSRAM 200 MHz，**仅跑 JPEG**）：

| 分辨率 | 输入格式 | 色度子采样 | fps |
|---|---|---|---|
| 1920×1080 | RGB565 | YUV420 | **40** |
| 1920×1080 | RGB565 | YUV422 | 36 |
| 1920×1080 | RGB888 | YUV422 | 26 |
| 1280×720 | RGB565 | YUV420 | **88** |
| 1280×720 | RGB565 | YUV422 | 81 |

板子是 **32 MB PSRAM @ 250 MHz + 400 MHz CPU**（优于测试条件）⇒ 1080p MJPEG 编码能力 **≥40 fps**，对 30 fps 约有 33% 余量。

### 11.3 USB 侧的每帧预算（真正的门槛）

- 等时端点（本栈**不支持**高带宽多事务，见第 2 节）= 1024 B/微帧 × 8000 微帧/s = **8.192 MB/s**
- 1080p30 ⇒ 8.192 MB/s ÷ 30 fps = **273 KB/帧**（扣掉 UVC 载荷头约 1%，净约 270 KB）
- **LVGL 生成的 UI 以大面积纯色 + 文字为主，JPEG 压缩率极高**，1080p 通常在几十 KB ~ 200 KB ⇒ 落在预算内
- ⚠️ 若 UI 内嵌照片 / 视频画面，单帧会迅速逼近甚至突破 273 KB，此时必须走 12.4 的 bulk，或降低分辨率/质量

### 11.4 关键利好：UVC 可以走 Bulk 端点，直接拆掉 273 KB 天花板

上游 TinyUSB 的 UVC 类**同时支持 Bulk 与等时**（`video_device.c:858` 注释 "Open bulk or isochronous endpoints"；`:864` 判等时；`:873` `TU_VERIFY(TUSB_XFER_BULK == ep->bmAttributes.xfer)` 后走 `usbd_edpt_open`）。

- 把视频流端点声明为 **Bulk** ⇒ 上限从 8.192 MB/s 提到 **~35 MB/s**（USB 2.0 HS 实测 bulk 量级），30 fps 下每帧预算变成 **~1.1 MB**，天花板基本消失
- ⚠️ 但 **Windows 11 收件箱 UVC 驱动是否接受 Bulk 视频流需要实测**：UVC 规范允许 bulk、Linux `uvcvideo` 支持明确，Windows 侧没有官方文档背书（[MS UVC 实现指南](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/uvc-camera-implementation-guide) 只讲格式类型与设备注册，不讲传输类型）
- **建议策略**：先按**等时**实现（够用、最稳），把 bulk 作为"余量升级项"单独实测；两者描述符只差 `bmAttributes` 与 `wMaxPacketSize`，切换成本很低

### 11.5 真正的瓶颈：LVGL 渲染 + PSRAM 带宽，而不是 USB

- 1080p RGB565 单帧 = **4.15 MB**
- 全屏刷新 30 fps ⇒ LVGL 写 124 MB/s + JPEG 编码器读 124 MB/s ≈ **250 MB/s 的 PSRAM 流量**
- 官方测试（PSRAM 200 MHz）能跑 40 fps，说明读取侧可承受约 166 MB/s；**250 MB/s 会逼近甚至超过 PSRAM 实际可用带宽**
- LVGL 全屏重绘 **2.07 M 像素/帧** 本身也很吃 CPU
⇒ **设计原则：UI 尽量局部刷新**（LVGL 只重绘脏区，静态 UI 渲染成本几乎为零），避免全屏动画；必要时用 **PPA 硬件缩放**——LVGL 按 960×540 渲染，再由 PPA SRM 放大到 1080p，LVGL/PSRAM 成本直接减半。

### 11.6 建议目标档位

| 档位 | 评价 |
|---|---|
| **1080p30 MJPEG** | **主推**。编码器余量约 33%；USB 每帧预算靠 UI 内容天然满足。但必须先实测实际帧体积与 LVGL 负载 |
| 720p30 MJPEG | **稳妥档**。编码器 88 fps（余量近 3 倍），每帧预算对 720p 非常宽裕 |
| 1080p30 YUY2 未压缩 | 放弃（物理不可能） |
| 1080p30 且要画质与余量 | 走 UVC **bulk**（需实测 Win11），或走**网络流**（hello_p4 已有 `README_CAMERA_TCP.md` 可复用） |

### 11.7 一句话回答

**1080p@30 有机会，但只有 MJPEG 这一条路**：未压缩在 USB 2.0 上物理不可行；MJPEG 有硬件编码器撑腰且余量够，前提是 UI 画面以纯色/文字为主、LVGL 采用局部刷新，并且**先把实测帧体积跑出来**再定质量参数。
