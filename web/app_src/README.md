# web/app_src · 单文件网页应用的源码

`web/app.html` 是**生成物**（单文件、无外部依赖、可从 SD 卡 `file://` 双击打开）。
本目录是它的**源码**，改代码请改这里，然后重新生成：

```powershell
python tools/build_web.py          # 生成/更新 web/app.html
python tools/build_web.py --check  # 只校验 app.html 与源码是否同步（不一致 exit 1）
node   web/app_src/selftest.mjs    # 离线自检（161 项断言，exit 0 = 全绿）
```

## 目录结构

```
web/app_src/
├── app.html.tpl        页面骨架（占位符：/*@STYLE@*/ 与 <!--@SCRIPTS@-->）
├── app.css             深色主题样式（原生 CSS，无外部字体/图片）
├── js/
│   ├── 05_bridge.js    把内联的 proto.js/file.js 顶层绑定收敛成 window.MP.*
│   ├── 10_util.js      格式化 / 路径 / 错误文本（纯函数）
│   ├── 20_usb.js       WebUSB 传输层：requestDevice → open → selectConfiguration →
│   │                   claimInterface(1)/claimInterface(2) → 两个独立持续读循环 → OUT 串行队列
│   ├── 30_link.js      链路会话胶水：LinkSession(tick/HELLO/PING) + FileClient(FILE 通道)
│   ├── 40_video.js     VIDEO 通道：消息分片 → 帧重组（frame_seq/chunk_index）→
│   │                   MJPEG(FFD9) 完整性判定 → Blob → createImageBitmap → canvas（最新帧覆盖）
│   ├── 50_files.js     FILE 通道高层语义：LIST 分页聚合 / 下载 / 上传 / mkdir / rename / delete /
│   │                   一键写 PC2P4.TXT 并回读校验
│   ├── 60_ui.js        DOM 绑定：视频面板 / TELEM 面板 / LOG 终端 / TF 卡浏览器 / 提示区
│   └── 90_boot.js      启动 + 初始化失败兜底（异常显示在 #boot-error）
└── selftest.mjs        离线自检（见下）
```

## 为什么内联 proto.js / file.js

`file://` 下 Chrome **禁止**加载外部 ES module / 外部脚本 / 外部样式，也不能 fetch 本地文件，
所以页面必须是"一个 HTML 文件里什么都有"。

`web/proto.js`（I0 协议权威实现）与 `web/file.js`（FILE 客户端）**不被改写**：
`tools/build_web.py` 只做 `strip_module()`（删 `import`/`export`、补 `'use strict'`，其余逐字节保留），
把它们作为 `<script id="wu-proto-inline">` / `wu-file-inline` 内联。
`selftest.mjs` 会**重新跑一遍同样的 strip 算法**并与 `app.html` 里的块逐字节比较 ——
任何一方漂移都会 FAIL。改协议 = 改 proto.js（需要 Lead 批准），然后重新生成。

## 页面操作（真机）

1. 插好 P4（VID 0x303A / PID 0x4103），等设备枚举完成；
2. Chrome / Edge 打开 `file:///<路径>/app.html`（`file://` 是安全上下文，WebUSB 可用）；
3. 点「连接设备」→ 弹窗里选 **MeidaP4 Composite** → 允许；
4. 页面自动 claim IF1（视频 EP 0x83 IN）与 IF2（控制 EP 0x84 IN / 0x04 OUT），
   等设备发 `HELLO` → 自动回 `HELLO_ACK` → 链路 up → 画面出现；
5. TF 卡：自动列根目录，面包屑/进入/下载/上传/新建目录/重命名/删除；
   「一键写入 PC2P4.TXT」会写入 `/PC2P4.TXT`（带时间戳）并回读逐字节校验，
   用来证明 PC 与 P4 的文件级共享。

## 离线自检覆盖什么

1. 单文件约束（无 `<script src>` / `<link>` / `<img>` / `type=module` / 顶层 `import` / `fetch` / 外链）；
2. `app.html` 与 `web/app_src/**` 逐字节同步（含 sha256 标记 + `build_web.py --check`）；
3. 内联镜像与 `proto.js` / `file.js` 逐字节一致（重算 strip 算法）；
4. 把全部脚本按顺序放进 node `vm`（假 DOM）执行：初始化无异常、`MP.*` 齐备；
5. `tools/proto_vectors.json` 全部 29 个向量（inline 镜像 / proto.js / 期望值三方对照）；
6. 假设备的页面逻辑环回：
   - WebUSB：claim 顺序/端点发现/ZLP 计数/OUT 串行/IF1 失败降级/IF2 失败致命/错误文案；
   - 视频：多分片重组、`frame_seq` 丢帧、缺 chunk、垃圾字节 resync、非 MJPEG 忽略、重绘；
   - FILE：真 `LinkSession`↔`LinkSession` 环回 + 内存设备端（**LIST 按条目序号翻页**），
     列目录/空目录/收尾页/上传/下载/mkdir/rename/delete/写 PC2P4.TXT 回读/ENOENT/
     ZLP/200 ms 重传/坏设备兼容；
7. 静态一致性：JS 引用的 DOM id 都存在且不重复；
8. 验收关键词（`claimInterface(1)`/`claimInterface(2)`/VIDEO/FILE/PC2P4.TXT…）。

## 只能等真机验证的项（离线 PASS ≠ 真机通过）

- IF1（视频）的 WinUSB/WCID 绑定是否生效 → Chrome `claimInterface(1)` 是否成功（失败会降级并给中文提示）；
- 真机零长度读（ZLP）频率、持续读吞吐、视频实际帧率/码率/延迟；
- 设备侧 FILE 服务的真实响应（尤其 LIST 的 `offset` 分页、OPEN_RSP 9 B、EBUSY/ENOENT 时机）；
- 拔插/复位后链路重连路径（依赖设备每 2 s 重发 HELLO 的收敛行为）。

## 已知限制（有意不做，避免超出本轮范围）

- 只解码 **MJPEG**（设备 fmt=0）；H.264（fmt=2）只统计不显示，留给下一阶段 WebCodecs。
- `CREDIT` 帧只记录不据以调整窗口（`proto.js` 同样未实现，属已登记的"未实现项"）。
- 不解析 MTP 卷：TF 卡浏览走 vendor FILE 通道（与 MTP 并存，各自独立）。
- 不支持 Windows 之外的浏览器（Firefox/Safari 没有 `navigator.usb`）。
