<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<title>MeidaP4 控制台 · WebUSB 视频 / TF 卡</title>
<style>
/*@STYLE@*/
</style>
</head>
<body>

<header class="topbar">
  <div class="brand">
    <span class="logo">MP</span>
    <div class="brand-text">
      <h1>MeidaP4 控制台</h1>
      <p>WebUSB · 视频画面 / TF 卡文件 / LOG / 遥测</p>
    </div>
  </div>
  <div class="conn">
    <span class="pill" id="pill-usb" title="WebUSB 设备与接口状态">USB 未连接</span>
    <span class="pill" id="pill-link" title="I0 链路（HELLO / PING）状态">链路 —</span>
    <span class="pill" id="pill-video" title="视频通道状态">视频 —</span>
    <button class="btn primary" id="btn-connect">连接设备</button>
    <button class="btn" id="btn-disconnect" disabled>断开</button>
  </div>
</header>

<main class="layout">

  <!-- ============================ 视频画面 ============================ -->
  <section class="card video-card">
    <div class="card-h">
      <h2>视频画面</h2>
      <span class="dim" id="video-hint">等待连接…</span>
    </div>
    <div class="canvas-wrap" id="canvas-wrap">
      <canvas id="video-canvas"></canvas>
      <div class="placeholder" id="canvas-placeholder">
        <div class="ph-icon">▶</div>
        <p>尚未收到视频帧</p>
        <p class="dim">点右上角「连接设备」→ Chrome 弹窗里选中 MeidaP4 Composite</p>
      </div>
    </div>
    <div class="stats">
      <div class="stat"><b id="v-res">—</b><span>分辨率</span></div>
      <div class="stat"><b id="v-fps">—</b><span>帧率</span></div>
      <div class="stat"><b id="v-kbps">—</b><span>码率</span></div>
      <div class="stat"><b id="v-avg">—</b><span>平均帧字节</span></div>
      <div class="stat"><b id="v-drop">—</b><span>丢帧</span></div>
      <div class="stat"><b id="v-dec">—</b><span>已解码</span></div>
    </div>
    <div class="hintline" id="v-hdr">视频头：—</div>
  </section>

  <!-- ============================ 设备状态 ============================ -->
  <section class="card status-card">
    <div class="card-h">
      <h2>设备状态</h2>
      <span class="dim" id="status-hint">等待 HELLO / TELEM（36 B 定长）</span>
    </div>
    <div class="telem-grid">
      <div><span>运行时间</span><b id="t-uptime">—</b></div>
      <div><span>空闲堆</span><b id="t-heap">—</b></div>
      <div><span>空闲 PSRAM</span><b id="t-psram">—</b></div>
      <div><span>USB 接收</span><b id="t-in">—</b></div>
      <div><span>USB 发送</span><b id="t-out">—</b></div>
      <div><span>视频码率</span><b id="t-kbps">—</b></div>
      <div><span>视频丢帧</span><b id="t-drop">—</b></div>
      <div><span>视频 FPS</span><b id="t-fps">—</b></div>
      <div><span>TF 卡</span><b id="t-sd">—</b></div>
      <div><span>Wi-Fi</span><b id="t-wifi">—</b></div>
    </div>
    <div class="counters" id="diag">
      <span>帧 <b id="d-frames">0</b></span>
      <span>零长读 <b id="d-zlp">0</b></span>
      <span>CRC 错 <b id="d-crc">0</b></span>
      <span>重同步 <b id="d-resync">0</b> B</span>
      <span>坏帧头 <b id="d-bad">0</b></span>
      <span>重传 <b id="d-retx">0</b></span>
      <span>窗口占用 <b id="d-win">0</b></span>
      <span>文件事务 <b id="d-file">0</b></span>
    </div>
    <div class="row">
      <button class="btn small" id="btn-ping">发 PING</button>
      <span class="dim" id="ping-result">—</span>
    </div>
    <details class="help">
      <summary>使用步骤 / 排障</summary>
      <ol>
        <li>USB 线插到 P4 的 <b>高速口</b>，等设备枚举完成（资源管理器里出现 MeidaP4 的 MTP 卷）。</li>
        <li>点「连接设备」→ Chrome 弹窗里选中 <b>MeidaP4 Composite</b>（VID 303A / PID 4103）→ 允许。</li>
        <li>页面会自动 claim <b>IF1（视频 EP 0x83 IN）</b> 与 <b>IF2（控制 EP 0x84 IN / 0x04 OUT）</b>，
            然后等设备发 <code>HELLO</code>；收到后自动回 <code>HELLO_ACK</code> 并开始出画面。</li>
        <li>claim IF1 失败 ⇒ 固件这一版没把 IF1 绑到 WinUSB（WCID）：换 Lead 刚烧的固件、
            <b>拔插一次</b>、并关闭其它占用该设备的 Chrome 标签页。</li>
        <li>RFC 提醒：只有 Chrome / Edge（Chromium）支持 WebUSB；Firefox / Safari 没有 <code>navigator.usb</code>。</li>
      </ol>
    </details>
  </section>

  <!-- ============================ LOG 终端 ============================ -->
  <section class="card log-card">
    <div class="card-h">
      <h2>LOG 终端</h2>
      <div class="card-tools">
        <label class="chk"><input type="checkbox" id="chk-autoscroll" checked> 自动滚动</label>
        <button class="btn tiny" id="btn-log-clear">清屏</button>
      </div>
    </div>
    <div class="log" id="log"></div>
  </section>

  <!-- ============================ TF 卡文件 ============================ -->
  <section class="card files-card">
    <div class="card-h">
      <h2>TF 卡文件</h2>
      <div class="card-tools">
        <button class="btn tiny" id="btn-files-refresh">刷新</button>
        <button class="btn tiny" id="btn-files-up">上级</button>
      </div>
    </div>
    <div class="crumbs" id="crumbs"></div>
    <div class="files-toolbar">
      <button class="btn small" id="btn-upload">上传文件…</button>
      <input type="file" id="file-input" multiple hidden>
      <button class="btn small" id="btn-newdir">新建目录</button>
      <button class="btn small" id="btn-rename" disabled>重命名</button>
      <button class="btn small danger" id="btn-delete" disabled>删除</button>
      <button class="btn small accent" id="btn-write-test">一键写入 PC2P4.TXT</button>
    </div>
    <div class="dim files-status" id="files-status">未连接（或尚未握手）</div>
    <div class="table-wrap">
      <table>
        <thead>
          <tr><th>名字</th><th class="num">大小</th><th>类型</th><th class="act">操作</th></tr>
        </thead>
        <tbody id="file-list"></tbody>
      </table>
    </div>
    <p class="dim tiny-note">
      LIST 按协议翻页：<code>LIST_REQ.offset</code> = 起始条目序号，每响应 ≤3 条（261 B/条），
      主机递增 offset 直到 0 条或 &lt;3 条。
    </p>
  </section>

</main>

<footer class="messages" id="messages" aria-live="polite">
  <div class="msg-head"><span>提示 / 错误</span><button class="btn tiny" id="btn-msg-clear">清空</button></div>
  <div class="msg-list" id="msg-list"></div>
</footer>

<pre id="boot-error" class="boot-error" style="display:none"></pre>

<!--@SCRIPTS@-->
</body>
</html>
