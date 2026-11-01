# ESP32-P4 WebUSB 方案计划书（v2 · 取代 v1 的 UVC 路线）

> 前置结论见 [可行性评估](./usb-composite-feasibility.md)。本文只写「要做什么、怎么做、引入什么配置」。
> 硬件：ESP32-P4 + C6 · USB 2.0 高速口（封装管脚 49/50）· 32 MB PSRAM @250 MHz · 32 MB Flash · TF 卡（SDMMC Slot 0 / GPIO39~44）· Win11 主机。

---

## 0. 架构定案

**单 USB 高速口，一个复合设备，三类接口：**

| 接口 | 用途 | 谁在用 |
|---|---|---|
| **MTP** | SD 卡**文件级**共享（非独占） | Windows 资源管理器 / 任何 MTP 客户端 |
| **Vendor + WebUSB** | 主通道：视频、串口终端、文件浏览、WiFi 配置、遥测 | 浏览器网页 |
| **NCM** | 系统级上网（PC 真正借 ESP 上网） | 操作系统网络栈 |

**砍掉 UVC 和 CDC**：
- 砍 UVC → 用浏览器画布承载视频，顺带解禁**硬件 H.264 编码器**（1080p30 只要 ~0.3 MB/s，未压缩则需 124 MB/s 物理不可能）。且 UVC 要占 2 个 IN 端点，保留会直接超预算。
- 砍 CDC → 串口终端搬到网页里，省 3 个端点。
- 保留 NCM 是**必需**的：浏览器拿不到"给整个系统用的网卡"，只有 NCM/RNDIS 能做到。

**页面分发**：单文件 HTML + 原生 JS（无构建步骤）放进 **SD 卡根目录**，经 MTP 拷到 PC 后打开。
**`file://` 也是安全上下文**（MDN [Secure contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Secure_Contexts) 的表格里明确列为 ✅ Secure），所以**双击本地 HTML 就能用 WebUSB**；`http://localhost` 同样可以。
⚠️ 唯一不行的是 `http://192.168.x.x`（ESP 经 Wi-Fi 直接供页）——非安全上下文，`navigator.usb` 直接不存在。

### 0.1 NCM 定案：走 **NAPT 路由**（不采用 L2 桥接）

USB 网卡侧由 P4 做**网关**：NCM netif 上一个私有网段 + 内置 DHCP 服务器，PC 落在 P4 后面的私网；P4 用 lwIP NAPT 把 PC 的流量 NAT 出去、经 Wi-Fi STA（C6 射频）上网。

| | **NAPT 路由（选定）** | L2 桥接（仅作降级备选） |
|---|---|---|
| 上游 AP 看到几个客户端 | **1 个**（只有 P4） | 2 个（P4 + PC），**依赖 AP 肯再发一个租约** |
| 酒店 / 企业 / 访客 Wi-Fi | ✅ 不受影响 | ❌ 每个 MAC 都要过门户认证 → PC 上不了网 |
| PC 侧子网冲突 | ✅ 独立私网 | ❌ PC 拿到与自身网卡同网段的 IP → Windows 双路由，时好时坏 |
| 隔离性 / 隐私 | ✅ PC 不暴露在上游内网 | ❌ PC 直接出现在上游 LAN |
| 代码量 | 多一个 DHCP server + NAT 配置 | 少（纯转发） |
| 官方先例 | ✅ esp_hosted `examples/wifi/apsta` = `WIFI_MODE_APSTA + LWIP NAPT` | ✅ `tusb_ncm` 示例 |

**三条决定性理由**：① 上游策略无关性 —— 上游只有 1 个客户端，酒店/企业 Wi-Fi 也能用；② 避免 PC 出现"同网段双路由"这类极难排查的间歇故障；③ Espressif 在同一套栈（hosted + NAPT）里已跑通，不是自创路径。

**具体配置**
- `CONFIG_LWIP_IP_FORWARD=y` + `CONFIG_LWIP_IPV4_NAPT=y`（⚠️ hello_p4 里这两项是**关**的，属新增项）
- NCM netif 用不常见私网：**`192.168.77.0/24`，P4 自身 `192.168.77.1`**（避开 192.168.0/1/4.x 与 10.x / 172.16–31.x；做成可配置）
- NCM netif 上跑 `esp_netif` DHCP server（`ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP` + 静态发送回调），参考 `examples/network/sta2eth/main/usb_ncm_iface.c`
- **DNS 不做代理**：DHCP option 6 直接下发上游 AP 给 STA 的 DNS（`esp_netif_get_dns_info`）
- ⚠️ **NAPT 只管 IPv4**：USB netif 上**关掉 IPv6（RA / DHCPv6）**，否则 PC 拿到 IPv6 地址却没有回程路由，表现为"部分站点卡住"
- **降级开关**：保留 L2 桥接作为编译期选项（`CONFIG_APP_USBNET_MODE_BRIDGE`），NAPT 出问题时一键切换

---

## 1. 冻结：接口与端点分配

| 接口 | 功能 | IN 端点 | OUT |
|---|---|---|---|
| MTP | SD 卡文件共享 | `INT IN`(事件) + `BULK IN` | `BULK OUT` |
| Vendor | WebUSB：视频 + 控制/文件/终端 | `BULK IN`(视频，独占) + `BULK IN`(控制复用) | `BULK OUT` |
| NCM | USB 网卡 | `INT IN`(通知) + `BULK IN` | `BULK OUT` |
| **合计** | | **6 个 + EP0 = 7/8**（余 1） | **3 个** |

- MTP 端点布局已从源码确认：`esp_tinyusb/usb_descriptors.c:252` → `TUD_MTP_DESCRIPTOR(..., 0x80|EPNUM_MTP_EVT, 64, 1, EPNUM_MTP, 0x80|EPNUM_MTP, 64)`，即 **1 事件中断 IN + 1 bulk IN + 1 bulk OUT**。
- 全程 **只用 bulk/int，不用等时** → 彻底绕开等时端点 8.192 MB/s 天花板和"高带宽等时不支持"的坑。
- HS TX FIFO 共 4 KB：本方案需求约 2.6 KB，且 H.264 码率极低 → **FIFO 与带宽都不再是约束**。

---

## 2. 冻结：四个契约（并行开发的前提）

| 契约 | 定位 | 作用 |
|---|---|---|
| **I0 · 线协议规约** | `docs/protocol.md` + `components/webusb_proto/` + `web/proto.js` | **最关键**：同一套帧格式被 C 和 JS 各实现一次。规定：帧头（magic/通道号/长度/序号）、通道划分（VIDEO / CTRL / FILE / LOG / TELEM）、流控与重传、断线重连。先定死再并行 |
| **I1 · 功能注册** | `components/usb_composite/` | 各功能注册自己的描述符片段与初始化；复合层统一组装、分配端点与 FIFO。**任何工作流都不直接改复合描述符** |
| **I2 · 帧源** | `components/video_pipe/include/` | `get_frame(ctx, &data, &len, fmt, timeout_ms)`：视频管线产帧，WebUSB 编码器消费 |
| **I3 · 网络桥接** | `components/usb_net/include/` | USB 网卡 ↔ `esp_netif` 解耦 |

---

## 3. 目标清单（按批，不设工期）

### 批 1 · 基建 + 冒烟（Lead）
| 目标 | 产出 | 验收 |
|---|---|---|
| 板级配置 | `sdkconfig.defaults`、自定义 `partitions.csv` | 启动打印 PSRAM 32 MB；`idf.py build` 干净 |
| 依赖清单 | `main/idf_component.yml` + 提交 `dependencies.lock` | 版本锁定 |
| 一键脚本 | `tools/build.ps1` / `flash.ps1`（显式加载 EIM profile） | 新终端一条命令 build+flash+monitor |
| **冒烟 A** | 单 Vendor 接口 + WebUSB | Win11 Chrome/Edge 里 `navigator.usb.requestDevice` 能选中并 `claimInterface` 成功 |
| **冒烟 B** | 复合（MTP + Vendor） | 资源管理器出现 MTP 设备 **且** 网页仍能 claim vendor 接口（验证"受保护类按接口判定"在实现层成立） |

### 批 2 · 三条并行工作流
| 工作流 | 写作用域 | 目标 | 验收 |
|---|---|---|---|
| **WS-storage** | `components/sd_storage/`, `components/mtp_backend/` | SD（Slot 0 / 4-bit / 40 MHz / LDO4 供电）+ FATFS + MTP 注册 | PC 里能浏览/读写 SD 卡；**同时** P4 侧 `fopen` 同一文件成功（证明共享而非独占） |
| **WS-net** | `components/net_hosted/`, `components/usb_net/` | esp_hosted 3.0.7 + NCM + `esp_netif` DHCP server + NAPT | PC 经 USB 网卡能 ping 外网与解析 DNS |
| **WS-webusb** | `components/webusb_proto/`, `components/usb_vendor/`, `web/` | 线协议 + vendor 接口 + 网页骨架（连接、终端、遥测） | 网页能连设备、跑通回环与日志通道；协议文档与实现一致 |

### 批 3 · 视频管线
| 目标 | 产出 | 验收 |
|---|---|---|
| LVGL 离屏渲染 | `components/ui/`：RGB565 帧缓冲（PSRAM）+ **局部刷新** | UI 可辨识；刷新耗时统计 |
| **阶段一：MJPEG** | `components/video_pipe/`：LVGL → PPA → 硬件 JPEG → I2 | 网页画布出图；实测每帧字节数 |
| **阶段二：H.264** | 硬件 H.264（`esp_h264`，最高 1080p/60fps/64K–10Mbps）+ WebCodecs `VideoDecoder` | **720p30 先达标**，再冲 1080p30；延迟 <1 帧；码率实测回填 |

### 批 4 · 集成与定型
复合四接口组装（含 Windows 侧 per-function WCID）· 四路并发压测（边传文件 + 边上网 + 边看视频 + 边刷 UI）· 拔插/重连健壮性 · 供电与热稳定 · 把实测数字回填文档。

---

## 4. 从 hello_p4 引入的配置（逐项）

### 4.1 `sdkconfig.defaults`（可直接抄这 3 行起步）
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_250M=y
```

### 4.2 从 hello_p4 的 `sdkconfig` 抄入的项
| 类别 | 关键项 |
|---|---|
| Flash | `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y` |
| PSRAM | `SPIRAM_BOOT_INIT` / `SPIRAM_BOOT_HW_INIT` / `SPIRAM_USE_MALLOC` / `SPIRAM_MEMTEST` / `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` / `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` |
| Wi-Fi 远端 | `ESP_WIFI_REMOTE_IN_IDF` / `ESP_WIFI_REMOTE_ENABLED` / `ESP_WIFI_REMOTE_LIBRARY_HOSTED` / `SLAVE_IDF_TARGET_ESP32C6` |
| ESP-Hosted | `ESP_HOSTED=y` · `ESP_HOSTED_HOST=y` · `HOST_TYPE_MCU` · `CP_TARGET_ESP32C6` · `AUTO_CALL_INIT_BEFORE_APP_MAIN` · `HOST_AUTO_FEAT_INIT` · `HOST_CORE` · `HOST_CP_RESET_STRATEGY_ALWAYS` · `HOST_CP_RESET_SETTLE_MS=1500` · `HOST_CP_BRINGUP_TIMEOUT_MS=5000` · `HOST_TRANSPORT_RESTART_ON_FAILURE` · `HOST_LOG_LEVEL_INFO` · `HOST_RPC_TIMEOUT_MS=5000` · `HOST_DEFLT_TASK_STACK_BYTES=4096` · `HOST_TO_ESP_WIFI_DATA_THROTTLE`（高/低阈值 80/60）· `P4_DEV_BOARD_NONE=y` |
| FATFS | `FATFS_LFN_HEAP` · `FATFS_MAX_LFN=255` · `FATFS_API_ENCODING_UTF_8` · `FATFS_CODEPAGE_936` · `FATFS_VOLUME_COUNT=2` · `FATFS_SECTOR_4096` · `FATFS_PER_FILE_CACHE` |
| 其他 | `ESP_DEFAULT_CPU_FREQ_MHZ_400` · `ESP32P4_REV_MIN_301` · `FREERTOS_HZ` |
| **NCM / NAPT（新增，见 0.1）** | `CONFIG_LWIP_IP_FORWARD=y` · `CONFIG_LWIP_IPV4_NAPT=y` · NCM netif 静态 IP `192.168.77.1/24` + DHCP server · USB netif 关闭 IPv6 RA/DHCPv6 |

### 4.3 依赖
```
espressif/esp_wifi_remote: '>=1.6.4'     # 会传递引入 esp_hosted 3.0.7
espressif/esp_tinyusb:    '^2.3.0'       # MTP / NCM / Vendor 都靠它
lvgl/lvgl + espressif/esp_lcd_* / esp_jpeg / esp_h264  # 按批 3 再加
```
> hello_p4 里还引了 `espressif/esp_codec_dev`（板载 MIC/喇叭）——本方案用不到，**不引入**。

### 4.4 代码级要抄的东西（来自 `hello_p4/main/main.cpp`，52 KB）
- **SD 卡初始化**：`SDMMC_HOST_SLOT_0` / `slot_config.width = 4` / `max_freq_khz = SDMMC_FREQ_HIGHSPEED(40 MHz)` / `SDMMC_SLOT_FLAG_INTERNAL_PULLUP`。
- ⚠️ **`esp_ldo_regulator` 给 SD 供电**：hello_p4 头部注释明确「SD 卡：SDMMC slot 0，4 位，**片内 LDO4 供电**」。**漏了这步 SD 卡不会工作**，这是最容易踩的坑。
- **FATFS 挂载/格式化/基准测试**：另有 `main/main_sd_card.cpp.back` 现成实现（mount / format / `sdmmc_erase_sectors` / 对齐 DMA 缓冲的读写基准）。
- **Wi-Fi over hosted 初始化**（`esp_wifi` + `esp_netif` + `esp_event` + `nvs_flash` 的标准序列）。
- `main.cpp` 里的 I2C / ISP / MIPI-CSI 部分本方案**不需要**，只取 SD 与 Wi-Fi 两段。

### 4.5 必须偏离 hello_p4 的地方
- **分区表**：hello_p4 用 `PARTITION_TABLE_SINGLE_APP`（app 仅 ~1 MB）。本方案含 LVGL + H.264 + Wi-Fi + USB，**app 必须 ≥4 MB** → 改用自定义 `partitions.csv`（32 MB flash 空间充裕）。
- 建议分区：`app 4M` + `storage(fat) 8M`（片内存储，保留给配置/资源，**不作 MTP 卷**）+ 其余留 SD 卡。
- **MTP 只暴露 SD 卡一个卷**（已定案）。

---

## 5. WebUSB 落地要点

1. **Windows 首次绑定**：vendor 接口需要在固件里发 **MS OS 2.0 描述符（WCID）**，Windows 才会自动挂 `winusb.sys`；否则要用 Zadig 手动装一次。复合设备需要 per-function subset header（MTP 那一路由系统自带的 `wpdmtp.sys` 绑定，互不干扰）。
2. **协议设计要点**：定长帧头 + 通道复用；视频通道与大文件传输**分用不同 EP**（避免队头阻塞）；帧序号 + 丢弃策略（视频宁可丢帧不重传）；控制通道需要 ACK/重传；心跳与断线重连。
3. **视频两阶段**：先 MJPEG（JS 侧 `createImageBitmap` 直接解，零编解码复杂度）跑通端到端 → 再上 H.264（长度前缀 NAL + `VideoDecoder`，延迟更低、码率低两个数量级）。
4. **浏览器限制**：仅 Chromium 内核（Chrome/Edge）；Firefox / Safari 不支持。
5. **不要保留 UVC**：与网页方案是二选一（端点会超）。

---

## 6. 风险与降级

| 风险 | 降级预案 |
|---|---|
| 复合设备下 vendor 接口 claim 失败 | 退回"单 Vendor 配置"（网页文件管理替代 MTP，NCM 另开一版固件） |
| Windows WCID 自动绑定不生效 | Zadig 手动装 WinUSB（开发期可接受） |
| H.264 + WebCodecs 链路不顺 | 停留在 MJPEG 阶段（1080p30 需 ~4.5 MB/s，bulk 完全够） |
| NAPT 复杂 | 退 L2 桥接（PC 直接从上游 AP 取 IP） |
| MTP 在 Windows 体验差 | 网页内自带文件管理器（走 vendor 通道，全平台一致） |

---

## 7. 已定案的决策

| 决策 | 结论 |
|---|---|
| 视频档位 | **720p30 起步，1080p30 为目标**（先 MJPEG 打通，再 H.264） |
| 共享上网架构 | **NAPT 路由**（0.1 节），L2 桥接仅作编译期降级选项 |
| SD 卡对外语义 | **MTP、单卷（仅 SD 卡）**，文件级共享，P4 与 PC 可同时访问 |
| 视频承载 | 走 **Vendor/WebUSB**，**放弃 UVC**（保留会超端点预算，且浏览器画布更自由） |
| 页面分发 | 单文件 HTML 放 SD 卡根目录，经 MTP 拷到 PC 后以 `file://` 打开（或 `http://localhost`） |

开工后若上述任何一项需要调整，改动都会落在 **I0 线协议规约**与复合层的端点分配表上——这两处是唯二的"牵一发动全身"位置。
