# 真机测试申请清单（设备未连接 —— 请按此表逐项授权）

> 生成者：Lead。上位：[usb-composite-plan.md](./usb-composite-plan.md) · [probe-report.md](./probe-report.md)
> 当前状态：**所有离线工作可继续；下表每一项都需要你把板子接上并授权。**
> 每条都写了「为什么必须上真机」「怎么测」「判定标准」——**离线 PASS ≠ 真机通过**。

---

## 0. 一句结论

离线能证明的只有三件事：**代码能编译**、**描述符字节与冻结表一致**、**C 与 JS 两侧实现逐字节相同**。
下面 4 组测试覆盖了**离线在原理上无法触及**的部分。

| 组 | 内容 | 阻塞了什么 | 优先级 |
|---|---|---|---|
| **T1** | 完整版首次上电：枚举 + WinUSB/WCID 绑定 + Chrome claim | 整条 WebUSB 主通道 | 🔴 **最高**（其余都建立在它能枚举之上） |
| **T2** | SD 卡（LDO4 供电）+ MTP 共享 | WS-storage 全部成果 | 🔴 高 |
| **T3** | NCM + NAPT 上网 | WS-net 全部成果 | 🟡 中 |
| **T4** | 视频管线（先 MJPEG 再 H.264） | plan 批 3 | 🟡 中 |

---

## T1 · 完整版首次上电（🔴 最高优先，一次接线可同时拿到 T2/T3 的前半段）

### 为什么必须上真机
`bcdDevice` / PID / MS OS 2.0 注册表属性的**应用时机**完全由 Windows 决定；描述符"形状对"不等于
"Windows 会应用它"。批 0 已经在这一条上栽过两次（`problem=31`、`problem=28`），而且**离线永远测不出来**。

### 开机前必做的两件事（否则会拿到假阴性）
1. **确认 PID = `0x4103`**（`CONFIG_TINYUSB_DESC_CUSTOM_PID` / `usb_composite` 默认身份）。
   它在 esp_tinyusb 自动 PID 空间（0x4000~0x407F）之外，不会撞缓存。**若你改过 `bcdDevice`，说明原因。**
2. 板子插到 PC 上后，**不要**用 Zadig；本方案靠固件自带 WCID。

### 怎么测
```powershell
# 1) 烧写（COM 口按实际改；探测期实测是 CH343 的 COM16）
pwsh -File tools/flash.ps1 -Port COM16
# 2) 后台抓 45 s 串口（⚠️ 注意：打开串口时的 DTR/RTS 抬动一定会复位芯片，
#    所以必须"让一次抓取把测试夹在中间"，不能用两次抓取做差 —— probe-report §3.3）
python tools/serial_capture.py --port COM16 --no-reset --seconds 45
```

### 判定标准（逐条都要）
| # | 检查 | 命令 / 位置 | 期望 |
|---|---|---|---|
| 1 | 自检 PASS | 串口 | `usb_composite: 描述符自检 PASS（N 条）` |
| 2 | 组装摘要与冻结表一致 | 串口 | `IF=5 FN=4(vendor 2) IN=6 OUT=3`；`FS cfg`/`HS cfg`/`BOS=57`/`MSOS=166` |
| 3 | FIFO 预算 | 串口 | `used=… / 1024 word`，**且 used ≤ 1024** |
| 4 | 枚举速率 | 串口 | `USBD Bus Reset : High Speed`（**不是** Full Speed） |
| 5 | **U8 复测** | 串口 + 设备管理器 | Windows 发 `C0 02 00 00 07 00 …`（`bRequest=0x02`、`wIndex=0x0007`） |
| 6 | 设备节点 | `Get-PnpDevice -Class USB,USBDevice,WPD` | 4 个节点全 `problem=0` |
| 7 | vendor 接口绑到 WINUSB | 同上 | `MI_01`/对应节点 `Service=WINUSB`、`CompatibleIds` 含 `USB\MS_COMP_WINUSB` |
| 8 | **device interface 真的注册出来了** | 注册表 | `Device Parameters` 下有**单数** `DeviceInterfaceGUID`（`REG_SZ`）= `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`，且 `DeviceClasses` 能查到该实例哈希 |
| 9 | **Chrome 能看到设备** | `chrome://usb-internals` 或网页 | `requestDevice` 列出 `MeidaP4 Composite` |
| 10 | **能 claim** | 网页 | `claimInterface` 成功，且 `DeviceInterfaceGUID` 存在 |

### ⚠️ 重点：U8 的更正与 A/B 实验
`docs/usb_contracts/frozen-allocation.md` §5.1 记录了 Lead 依据源码对 probe-report §5（U8）的**更正**：
模式 B（复合 + Function Subset Header + 单数 REG_SZ = **166 B**）**已经真机验证过**，
probe-report §5 的 U8 说法是错的（verifier 用 `tools/desc_lint.py` 独立复核确认）。

因此本版**默认照抄已验证形态**（`CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET=n`）。
若第 8/9/10 条**失败**，请做一个 A/B：
```
# 打开 Configuration Subset Header 再试一次
pwsh -File tools/build.ps1 -Mode prod -Slot main -ExtraDefaults ... # 或直接改 sdkconfig.<slot>-prod
#   CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET=y
```
并**务必同时递增 `CONFIG_USB_COMPOSITE_*` 的 bcdDevice**（Windows 只在安装时应用 MS OS 属性）。

---

## T2 · SD 卡 + MTP（🔴 高）

### 为什么必须上真机
`esp_ldo_regulator` channel 4 给 SD 供电的**电压值、上电时序、引脚**全部来自 hello_p4 的注释与代码，
**没有任何离线手段能验证**。plan §4.4 明确点名"漏了这步 SD 卡不会工作"。

### 判定标准
| # | 检查 | 期望 |
|---|---|---|
| 1 | LDO 起得来 | 串口无 `esp_ldo` 错误；SD 初始化成功 |
| 2 | 卡被识别 | 串口打印容量/`SDMMC` 速率；**4-bit / 40 MHz** |
| 3 | 资源管理器能浏览 | "此电脑"出现 `MeidaP4 SD Card`，能进目录看到文件 |
| 4 | **PC 能写** | 从 PC 拷一个文件进去，成功 |
| 5 | **同时 P4 能读同一个文件** | 这是 plan 批 2 的**核心验收**（文件级共享而非独占）：P4 侧 `fopen` 同一路径成功且内容一致 |
| 6 | 长文件 | ≥100 MB 拷贝不中断、不超时 |
| 7 | 中文文件名 | 建/读中文名文件正常（`FATFS_CODEPAGE_936` + UTF-8） |

### 需要你提供的信息
- 开发板型号 / 是否有原理图（尤其 **SD 卡走线是不是 SDMMC 4-bit**、**LDO4 实际电压**）
- 卡的类型与容量（SDSC/SDHC/SDXC）

---

## T3 · NCM + NAPT 上网（🟡 中）

### 为什么必须上真机
Windows 对 NCM 的原生驱动支持、NAPT 会话行为、DNS 透传、C6 固件版本握手——**全部只有真机能测**。
另外 `esp_hosted` 的 SDIO 用 **Slot 1**（CMD19/CLK18/D0~D3=14/15/16/17、复位 GPIO54），
SD 卡用 **Slot 0**，两者能否共存也是真机项。

### 判定标准
| # | 检查 | 期望 |
|---|---|---|
| 1 | C6 握手 | 串口打印 esp_hosted 版本（`3.0.7` 预期）；**且 `ESP_HOSTED_HOST_FEAT_WIFI=y` 真的生效** |
| 2 | STA 连上 | 串口打印 SSID / RSSI / IP |
| 3 | PC 出现网卡 | Windows 网络适配器多一个 USB 网卡（NCM） |
| 4 | PC 拿到 IP | DHCP 从 P4 拿到 `192.168.77.x`（默认网段） |
| 5 | PC 能 ping P4 | `ping 192.168.77.1` 通 |
| 6 | **PC 能 ping 外网** | 例如 `ping 8.8.8.8` 或 `ping www.baidu.com` |
| 7 | **DNS 解析** | `nslookup www.baidu.com` 返回（验证 DHCP option 6 透传上游 DNS） |
| 8 | **IPv6 已关** | PC 的该网卡**不应**拿到 IPv6 地址（否则会出现"部分站点卡住"） |
| 9 | 长跑 | 连续上网 30 min 不掉线 |

### 需要你确认
- PC 是 **Windows 10 还是 11**（决定 NCM 是否需要降级到 RNDIS）
- 是否有可用的 Wi-Fi（SSID/密码会写进 sdkconfig 还是运行时经网页下发？——当前设计是**运行时经网页下发**，见 `docs/protocol.md` `WIFI_CONNECT`）

---

## T4 · 视频管线（🟡 中，plan 批 3）

### 分两阶段，**先 MJPEG 打通再上 H.264**（plan §7 已定案）
| 阶段 | 判定标准 |
|---|---|
| **MJPEG** | 720p30：网页画布连续出图；`video_pipe` 统计里 `bytes_avg` / `fps_actual_x100` / `encode_us_avg` 有真实数字；**长时间不冻结、不花屏** |
| **H.264** | 先 720p30 达标，再冲 1080p30；延迟 < 1 帧；实测码率回填文档 |

⚠️ 视频还依赖一个**离线无法验证**的东西：LVGL 离屏渲染的实际耗时与 PSRAM 带宽。
plan §11.5 已判定**真正的瓶颈是 LVGL 渲染 + PSRAM 带宽，不是 USB** —— 请重点记录 `render_us_avg`。

---

## T5 · 供电与热稳定（plan §6 风险 4，**完整版必测**）

批 0 **完全没做**（probe-report §5 U6）。Wi-Fi 发射峰值 + PSRAM + SD 同时工作的电流可能超过
USB 2.0 总线的 500 mA，导致复位/枚举失败。

需要你提供：**板子是总线供电还是自供电？** 若总线供电，建议实测：
- 同时跑「传文件 + 上网 + 看视频 + 刷 UI」四路，持续 1 h，记录是否掉线/复位
- 万用表/USB 电流表读峰值电流

---

## 授权方式

请直接回复我：
1. **哪几组**可以测（建议至少 T1）
2. **COM 口**（探测期是 COM16）
3. **板子供电方式**与 **PC 的 Windows 版本**
4. **是否有可用 Wi-Fi**（T3 需要；凭据可运行时经网页下发，不必写进固件）

我会据此把要烧的镜像、要抓的串口日志、要跑的脚本一次性准备好再动手。
