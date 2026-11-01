# T2 归档（2026-10-31 / 11-01）：画面显示 + 共享 TF 卡

本目录是「T2」阶段的**成品快照**，对应 `docs/findings-t2-2026-10-31.md` 记录的那一版固件。

## 内容

| 文件 | 说明 |
|---|---|
| `MeidaP4-t2-final.bin` | 最终烧写镜像（app 分区用，`0x10000` 偏移） |
| `MeidaP4-t2-final.elf` | 对应 ELF（带符号，便于 `addr2line`/`nm` 复核） |
| `bootloader.bin` / `partition-table.bin` | 烧写所需另两个分区镜像 |

> ⚠️ 本目录内的 `.bin/.elf` 被根 `.gitignore` 的 `*.bin` / `*.elf` 规则忽略 —— 也就是说**默认不会进 git**。
> 需要把镜像一起入库时，请单独执行 `git add -f archive/t2-2026-10-31/*.bin`（或只保留本地归档）。

## 复现构建

```powershell
pwsh -File tools/build.ps1 -Mode prod -Slot flash      # -> build-flash-prod/MeidaP4.bin
pwsh -File tools/flash.ps1 -Mode prod -Slot flash -Port COM16
```

构建输入已全部入库：`sdkconfig.defaults` + `sdkconfig.prod.defaults` + `partitions.prod.csv` + `main/idf_component.yml` + `dependencies.lock`。
`build-*`、`sdkconfig.<slot>-prod` 都是生成物，已在 `.gitignore` 中忽略。

## 这一版包含的能力（真机已验证）

- **画面显示**：LVGL 9.2 离屏（1280×720 RGB565）→ 硬件 JPEG（4:2:0, q80）→ WebUSB VIDEO（IF1 / EP3）→ 网页 canvas。
  实测：设备侧 33.7 fps / ~16.9 Mbps / dropped=0；主机侧 32–34 fps、JPEG 全合法、gaps=0。
- **共享 TF 卡**：MTP（PC）↔ 设备 FILE 通道（网页/主机工具）↔ P4 自身，三者同一 FATFS 卷。
  实测：PC 经 MTP 写 `PC2P4.TXT` → 设备探针立刻读到；P4 心跳文件经 MTP 拉到 PC；190 KB 网页经 FILE 通道写入 SD 后 MTP 拉回哈希一致。
- **网页应用**：`web/app.html`（单文件、`file://` 双击可用）：视频画布 + TF 卡文件浏览器 + LOG/TELEM + 一键写 `PC2P4.TXT`。
- WiFi 共享按计划**未做**。

## 关键 bug 与协议修订（详见 docs）

真机暴露并修掉 8 个问题，其中 3 个是"离线/单侧工具会互相掩盖"的类型：
1. `VIDEO` 分片违反 §5.2（每个协议帧都塞了 16 B 头）→ 网页花屏/错位；
2. 引导期 `PING(seq=0)` + "无效帧也刷新超时" → 序号死锁（网页无画面 / 几百帧后卡死）；
3. `ACK` 帧占通道 seq → 丢一条 ACK 就永久死锁（`retx_exhausted` 循环）——**已修订协议 §6.5②**。

另外 5 个：IF1 缺 WCID（视频端点主机打不开）、`webusb_proto_ep_write` 丢 EP3、`usb_composite` 的合成 pending 死锁、
`link_up/link_down` 顺序 + 无符号差值下溢、视频一次部分写就整帧作废、`file_service` 误吞 MKDIR、`statvfs` 不存在。

## 验收工具

```powershell
# 视频：握手 + 收帧 + 存 JPEG + 校验 FFD8/FFD9
python tools/webusb_app.py --video-seconds 10 --save-frames
# FILE 通道：LIST 分页 / 写 / 读回 / 下载 / MKDIR
python tools/webusb_app.py --file-test
# MTP 双向（模拟资源管理器）
pwsh -File tools/mtp_copy.ps1 -Source out\PC2P4.TXT
pwsh -File tools/mtp_pull.ps1 -Name MeidaP4-P4-HEARTBEAT.txt -DestDir out
```
