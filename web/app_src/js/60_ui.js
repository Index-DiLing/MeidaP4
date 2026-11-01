// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · UI（原生 DOM，无框架）
//
// 结构：
//   ┌ 顶栏   连接状态 / 链路状态 / 视频状态 / 连接·断开
//   ├ 视频   画布 + 分辨率/帧率/码率/平均字节/丢帧/已解码
//   ├ 状态   TELEM 36 B 定长字段 + 诊断计数器 + 使用步骤
//   ├ LOG    设备 LOG 通道 + 本地事件（滚动、清屏）
//   └ 文件   TF 卡浏览器（面包屑 / 翻页列目录 / 下载 / 上传 / 新建目录 / 重命名 / 删除 / 一键写 PC2P4.TXT）
//
// 所有对设备数据的显示一律走 textContent（设备给的文件名不参与 HTML 解析）。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};
  var WU = MP.WU;
  var U = MP.util;

  var MAX_LOG_LINES = 800;
  var MAX_MSGS = 60;
  var STATS_PERIOD_MS = 400;

  var S = {
    usb: null,
    link: null,
    video: null,
    files: null,
    connected: false,
    busy: false,          // 连接/断开中
    filesBusy: false,
    usbAvailable: false,  // navigator.usb 是否存在（决定「连接设备」按钮可用性）
    linkUp: false,
    caps: 0,
    autoListed: false,
    cwd: '/',
    entries: [],
    selected: null,
    lastTelem: null,
    lastFrameAt: 0,
    lastFrameCount: 0,
    statsTimer: null,
    logLines: 0,
    initialised: false
  };

  function el(id) { return root.document ? root.document.getElementById(id) : null; }
  function setText(id, text) { var e = el(id); if (e) e.textContent = String(text); }
  function setDisabled(id, v) { var e = el(id); if (e) e.disabled = !!v; }
  function addClass(id, c) { var e = el(id); if (e && e.classList) e.classList.add(c); }
  function removeClass(id, c) { var e = el(id); if (e && e.classList) e.classList.remove(c); }

  var UI = MP.UI = { state: S };

  /* ================================================================ 日志/提示 */

  UI.log = function (level, text) {
    var box = el('log');
    if (!box) return;
    var doc = root.document;
    var line = doc.createElement('div');
    line.className = 'ln ' + (level || 'info');
    var ts = doc.createElement('span');
    ts.className = 'ts';
    ts.textContent = U.fmtClock();
    line.appendChild(ts);
    line.appendChild(doc.createTextNode(String(text)));
    box.appendChild(line);
    S.logLines++;
    while (S.logLines > MAX_LOG_LINES && box.firstChild) {
      box.removeChild(box.firstChild);
      S.logLines--;
    }
    var auto = el('chk-autoscroll');
    if (!auto || auto.checked) box.scrollTop = box.scrollHeight;
  };

  UI.toast = function (kind, text) {
    var box = el('msg-list');
    if (!box) return;
    var doc = root.document;
    var m = doc.createElement('div');
    m.className = 'm ' + (kind || 'info');
    var t = doc.createElement('time');
    t.textContent = U.fmtClock();
    m.appendChild(t);
    m.appendChild(doc.createTextNode(String(text)));
    box.appendChild(m);
    var kids = box.children || [];
    while (kids.length > MAX_MSGS && box.firstChild) box.removeChild(box.firstChild);
    box.scrollTop = box.scrollHeight;
  };

  function setPill(id, kind, text) {
    var e = el(id);
    if (!e) return;
    e.className = 'pill' + (kind ? ' ' + kind : '');
    e.textContent = text;
  }

  function hex32(n) { return '0x' + Number(n >>> 0).toString(16).toUpperCase().padStart(8, '0'); }

  function capsText(caps) {
    if (!caps) return '（无能力位）';
    var out = [];
    if (caps & WU.CAP.VIDEO_MJPEG) out.push('MJPEG');
    if (caps & WU.CAP.VIDEO_H264) out.push('H264');
    if (caps & WU.CAP.FILE) out.push('FILE');
    if (caps & WU.CAP.LOG) out.push('LOG');
    if (caps & WU.CAP.TELEM) out.push('TELEM');
    if (caps & WU.CAP.WIFI) out.push('WIFI');
    if (caps & WU.CAP.UI_CMD) out.push('UI_CMD');
    return out.length ? out.join(' · ') : '（无已知能力位）';
  }

  /* ================================================================ 状态刷新 */

  UI.updateButtons = function () {
    var fileReady = S.connected && S.linkUp && !!S.files && !S.filesBusy;
    setDisabled('btn-connect', !S.usbAvailable || S.connected || S.busy);
    setDisabled('btn-disconnect', !S.connected || S.busy);
    setDisabled('btn-files-refresh', !fileReady);
    setDisabled('btn-files-up', !fileReady || S.cwd === '/');
    setDisabled('btn-newdir', !fileReady);
    setDisabled('btn-upload', !fileReady);
    setDisabled('btn-write-test', !fileReady);
    var hasSel = fileReady && !!S.selected;
    setDisabled('btn-rename', !hasSel);
    setDisabled('btn-delete', !hasSel);
    setDisabled('btn-ping', !(S.connected && S.linkUp));
  };

  function renderTelem() {
    var t = S.lastTelem;
    if (!t) return;
    setText('t-uptime', U.fmtDuration(t.uptimeMs));
    setText('t-heap', U.fmtBytes(t.heapFree));
    setText('t-psram', U.fmtBytes(t.psramFree));
    setText('t-in', U.fmtBytes(t.usbInBytes));
    setText('t-out', U.fmtBytes(t.usbOutBytes));
    setText('t-kbps', U.fmtInt(t.videoKbps) + ' kbps');
    setText('t-drop', U.fmtInt(t.videoDropped));
    setText('t-fps', (t.videoFpsX100 / 100).toFixed(1));
    setText('t-sd', t.sdMounted ? '已挂载' : '未挂载');
    setText('t-wifi', t.wifiUp ? ('up ' + t.wifiRssi + ' dBm') : (t.wifiRssi ? ('down ' + t.wifiRssi + ' dBm') : 'down'));
    setText('status-hint', 'TELEM 36 B · 版本 v' + (S.link && S.link.session.deviceVersion !== null
      ? S.link.session.deviceVersion : '?') + ' · caps ' + capsText(S.caps));
  }

  function renderVideoStats() {
    if (!S.video || !S.connected) {
      setText('v-res', '—'); setText('v-fps', '—'); setText('v-kbps', '—');
      setText('v-avg', '—'); setText('v-drop', '—'); setText('v-dec', '—');
      setText('v-hdr', '视频头：—');
      return;
    }
    var v = S.video.snapshot();
    var h = v.header;
    setText('v-res', h ? (h.w + '×' + h.h) : '—');
    setText('v-fps', v.fps > 0 ? v.fps.toFixed(1) : '0.0');
    setText('v-kbps', U.fmtKbps(v.bytesPerSec));
    setText('v-avg', v.avgBytes ? U.fmtBytes(v.avgBytes) : '—');
    setText('v-drop', U.fmtInt(v.droppedFrames));
    setText('v-dec', U.fmtInt(v.framesDecoded));
    setText('v-hdr', '视频头：' + (h
      ? ('fmt=' + h.fmt + ' ' + (h.fmt === 0 ? 'MJPEG' : h.fmt === 1 ? 'RGB565' : 'H264') +
        ' 关键帧=' + (h.keyframe ? '是' : '否') + ' ' + h.w + '×' + h.h +
        ' fps=' + (h.fpsDen ? (h.fpsNum / h.fpsDen).toFixed(2) : '?') +
        ' frame_seq=' + h.frameSeq + ' bytes=' + h.bytes)
      : '—') +
      '  · 解码 ' + v.decodeMs + ' ms / 绘制 ' + v.lastDrawMs + ' ms' +
      '  · 消息 ' + U.fmtInt(v.messages) + ' 条 · 残帧丢弃 ' + U.fmtInt(v.droppedPartial) +
      ' · 解码失败 ' + U.fmtInt(v.decodeErrors));

    var now = Date.now();
    if (v.framesComplete > S.lastFrameCount) { S.lastFrameCount = v.framesComplete; S.lastFrameAt = now; }
    var fresh = S.lastFrameAt && (now - S.lastFrameAt < 1500);
    var wrap = el('canvas-wrap');
    if (wrap && wrap.classList) {
      if (v.hasFrame) wrap.classList.remove('empty');
      else wrap.classList.add('empty');
    }
    if (v.hasFrame) {
      setPill('pill-video', fresh ? 'ok' : 'warn', fresh ? '视频 出图中' : '视频 无新帧');
      setText('video-hint', fresh ? ('MJPEG ' + (S.video.lastHeader ? S.video.lastHeader.w + '×' + S.video.lastHeader.h : '') +
        ' · ' + v.fps.toFixed(1) + ' fps') : '视频通道已连上，但 1.5 s 内没有新帧（设备可能暂停推流）');
    } else if (S.usb && S.usb.claimed.video) {
      setPill('pill-video', 'warn', '视频 等首帧');
      setText('video-hint', '已 claim IF1（视频），等待第一帧…');
    } else {
      setPill('pill-video', 'err', '视频 不可用');
      setText('video-hint', 'IF1（视频）未 claim，只有控制通道可用');
    }
  }

  function renderDiagnostics() {
    var link = S.link ? S.link.snapshot() : null;
    var usb = S.usb ? S.usb.snapshot() : null;
    var vid = S.video ? S.video.snapshot() : null;
    if (!link || !usb) {
      ['d-frames', 'd-zlp', 'd-crc', 'd-resync', 'd-bad', 'd-retx', 'd-win', 'd-file'].forEach(function (id) {
        setText(id, '0');
      });
      return;
    }
    var frames = link.frames + (vid ? vid.decoderStats.frames : 0);
    setText('d-frames', U.fmtInt(frames));
    setText('d-zlp', U.fmtInt(usb.zeroLenReads.control + usb.zeroLenReads.video));
    setText('d-crc', U.fmtInt(link.crcErrors + (vid ? vid.decoderStats.crcErrors : 0)));
    setText('d-resync', U.fmtInt(link.resyncBytes + (vid ? vid.decoderStats.resyncBytes : 0)));
    setText('d-bad', U.fmtInt(link.badHeader + (vid && vid.decoderDiag ? vid.decoderDiag.badHeader : 0)));
    setText('d-retx', U.fmtInt(link.ctrl.retransmits + link.file.retransmits));
    setText('d-win', U.fmtInt(link.ctrl.unacked + link.file.unacked));
    setText('d-file', U.fmtInt(link.fileClient.requests) + (link.fileClient.pending ? ('（挂起 ' + link.fileClient.pending + '）') : ''));
  }

  UI.refreshStats = function () {
    try {
      renderTelem();
      renderVideoStats();
      renderDiagnostics();
    } catch (e) {
      // 统计刷新失败绝不能影响协议/读循环
      if (!S._statsErrShown) {
        S._statsErrShown = true;
        UI.log('warn', '统计面板刷新异常：' + U.errText(e));
      }
    }
  };

  /* ================================================================ 连接生命周期 */

  function hooks() {
    return {
      log: function (level, text) { UI.log(level, text); },

      onHello: function (info) {
        S.caps = info.caps >>> 0;
        var verOk = (info.version === WU.PROTO_VERSION);
        UI.log(verOk ? 'ok' : 'warn', '收到 HELLO：协议版本=' + info.version +
          '（本页实现 v' + WU.PROTO_VERSION + '）caps=' + hex32(S.caps) + ' [' + capsText(S.caps) + ']');
        if (!verOk) UI.log('warn', '⚠️ 协议版本不一致：帧格式可能对不上，若解析异常请核对 docs/protocol.md');
        UI.log('ok', '已回 HELLO_ACK（引导帧 seq=0/flags=0，不占可靠窗口）→ 链路 up');
        S.linkUp = true;
        setPill('pill-link', 'ok', '链路 up');
        setText('status-hint', 'caps ' + capsText(S.caps));
        UI.updateButtons();
        // 设备主导序号：收到 HELLO 说明设备已重置自己的 TX 序号，
        // 视频端点上的半帧字节不可再信 —— 丢掉流状态（不动累计统计）。
        if (S.video) S.video.resetStream();
        if (!(S.caps & WU.CAP.FILE)) {
          UI.log('warn', '设备的 caps 里没有 FILE 位：TF 卡文件功能会被设备拒绝（ENOTSUP）。');
        }
        // 自动列一次根目录：连上就能看到 TF 卡内容
        if (!S.autoListed && (S.caps & WU.CAP.FILE)) {
          S.autoListed = true;
          UI.refreshDir('/');
        }
      },

      onHelloAck: function () { UI.log('info', '收到 HELLO_ACK'); },

      onLink: function (s) {
        if (s.state === 'up') {
          S.linkUp = true;
          setPill('pill-link', 'ok', '链路 up');
        } else if (s.state === 'link_down') {
          S.linkUp = false;
          S.autoListed = false;          // 重连后要重新列一次目录（旧列表可能已失效）
          setPill('pill-link', 'warn', '链路 重连中');
          UI.log('warn', '链路 down（' + (s.reason || '3 s 内没有收到任何有效帧') + '）：已清窗口与拼接态。' +
            '设备会每 2 s 重发 HELLO，收到即自动恢复 —— 无需手动重连。');
          setText('files-status', '链路断开：文件操作已暂停，等待重连…');
        } else {
          S.linkUp = false;
          setPill('pill-link', 'warn', '链路 等待 HELLO');
        }
        UI.updateButtons();
      },

      onTelem: function (t) {
        S.lastTelem = t;
      },

      onLogLine: function (text) {
        UI.log('dev', '[设备] ' + text);
      },

      onCtrl: function (text) { UI.log('info', '[CTRL] ' + text); },

      onGap: function (text) { UI.log('warn', text); },

      onPong: function () { /* 手动 ping 的 RTT 由 Link.ping() 统计 */ },

      onUnexpected: function (text) { UI.log('warn', text); }
    };
  }

  UI.connect = async function () {
    if (S.busy) return;
    if (S.connected) { UI.log('warn', '已经连接了（要重连请先点「断开」）'); return; }
    S.busy = true;
    UI.updateButtons();
    setPill('pill-usb', 'warn', 'USB 连接中…');
    UI.log('info', '开始连接：requestDevice → open → selectConfiguration → claim IF1/IF2 …');

    var usb = null;
    try {
      usb = new MP.Usb({
        log: function (level, text) { UI.log(level, text); },
        onDisconnect: function (reason) { UI.onDeviceGone(reason); },
        onZlp: function (kind, n) {
          if (n === 1 || n % 128 === 0) {
            UI.log('info', '收到 0 字节读（ZLP，' + kind + ' 端点，累计 ' + n + ' 次）—— 合法事件，继续读');
          }
        }
      });
      var res = await usb.connect();

      S.usb = usb;
      S.video = new MP.Video({
        canvas: el('video-canvas'),
        onLog: function (level, text) { UI.log(level, text); },
        onFirstFrame: function (h) {
          // 先把占位层摘掉，再让 Video 绘制 —— 否则 canvas 在 display:none 下
          // clientWidth=0，首帧会被画成 1×1（下一帧才会正常）。
          removeClass('canvas-wrap', 'empty');
          UI.log('ok', '收到第一帧视频：' + h.w + '×' + h.h + '，' + h.bytes + ' B，frame_seq=' + h.frameSeq +
            '（MJPEG → createImageBitmap → canvas）');
        }
      });
      S.link = new MP.Link(usb, hooks());
      S.files = new MP.Files(S.link, {
        onProgress: function (p) { showProgress(p); },
        onNotice: function (text) { UI.log('warn', text); UI.toast('warn', text); }
      });

      // 端点 → 处理器（两个端点各自独立解码，绝不混流）
      usb.handlers.control = function (bytes) { S.link.feedControl(bytes); };
      usb.handlers.video = function (bytes) { S.video.feed(bytes); };

      S.connected = true;
      S.linkUp = false;
      S.autoListed = false;
      S.lastTelem = null;
      S.lastFrameCount = 0;
      S.lastFrameAt = 0;

      setPill('pill-usb', 'ok', 'USB 已连接');
      setPill('pill-link', 'warn', '链路 等 HELLO');
      S.link.start();
      S.link.helloHandshake();   // 主动 HELLO_ACK：强制设备 link_up 归零，避免与上一台主机残留序号死锁
      await usb.startReads();

      if (!res.video.ok) {
        setPill('pill-video', 'err', '视频 不可用');
        var hint = (res.video.error && res.video.error.hint) || '';
        UI.log('err', 'IF1（视频）不可用：' + U.errText(res.video.error));
        UI.toast('err', 'IF1（视频 EP 0x83）claim 失败，已降级为「仅控制模式」：\n' +
          'TF 卡文件 / LOG / 遥测可用，画面不可用。\n' + (hint || ''));
        setText('video-hint', 'IF1 未 claim（固件需包含 IF1 WCID / 拔插一次后重试）');
      } else {
        setPill('pill-video', 'warn', '视频 等首帧');
        setText('video-hint', '已 claim IF1（视频 EP 0x83），等待第一帧…');
      }

      UI.log('info', '已启动读循环，等待设备发 HELLO（设备每 2 s 重发，收到后本页自动回 HELLO_ACK）…');
      UI.toast('ok', 'USB 已连接。' + (res.video.ok ? '' : '（视频接口未 claim，见错误提示）'));
    } catch (e) {
      UI.log('err', '连接失败：' + U.errText(e));
      if (e && e.hint) UI.log('err', e.hint);
      UI.toast('err', '连接失败：' + U.errText(e) + (e && e.hint ? '\n' + e.hint : ''));
      // 连接中途失败时必须把已经 open()/被 claim 的资源释放掉，
      // 否则下一次「连接设备」会撞上"设备已被本页占用"（open() 抛错）。
      if (usb) {
        try { await usb.close(); } catch (ignore) { /* 尽力而为 */ }
      }
      await UI.disconnect(true);
      setPill('pill-usb', 'err', 'USB 未连接');
    } finally {
      S.busy = false;
      UI.updateButtons();
    }
  };

  UI.disconnect = async function (silent) {
    // 先把状态清空（并保存引用），再 await 关闭：
    // 这样"断开过程中点连接"不会看到半旧状态，也不会被重复关闭。
    var usb = S.usb;
    var link = S.link;
    S.usb = null;
    S.link = null;
    S.video = null;
    S.files = null;
    S.connected = false;
    S.linkUp = false;
    S.caps = 0;
    S.autoListed = false;
    S.entries = [];
    S.selected = null;
    S.cwd = '/';
    S.lastTelem = null;
    S.lastFrameCount = 0;
    S.lastFrameAt = 0;

    if (link) { try { link.stop(); } catch (ignore) { /* 忽略 */ } }
    if (usb) {
      try { await usb.close(); }
      catch (e) { UI.log('warn', '关闭设备时异常：' + U.errText(e)); }
    }

    setPill('pill-usb', '', 'USB 未连接');
    setPill('pill-link', '', '链路 —');
    setPill('pill-video', '', '视频 —');
    setText('status-hint', '等待 HELLO / TELEM（36 B 定长）');
    setText('files-status', '未连接（或尚未握手）');
    setText('video-hint', '等待连接…');
    ['v-res', 'v-fps', 'v-kbps', 'v-avg', 'v-drop', 'v-dec'].forEach(function (id) { setText(id, '—'); });
    setText('v-hdr', '视频头：—');
    ['t-uptime', 't-heap', 't-psram', 't-in', 't-out', 't-kbps', 't-drop', 't-fps', 't-sd', 't-wifi']
      .forEach(function (id) { setText(id, '—'); });
    renderFileList([]);
    renderCrumbs();
    addClass('canvas-wrap', 'empty');
    UI.updateButtons();
    if (!silent) UI.log('info', '已断开设备并释放接口');
  };

  UI.onDeviceGone = function (reason) {
    if (!S.connected) return;
    UI.log('err', '设备已断开：' + reason);
    UI.toast('warn', '设备已断开（拔出/复位）。重新插拔后点「连接设备」。');
    UI.disconnect(false);
  };

  /* ================================================================ 文件浏览器 */

  /** FILE 通道错误 → 中文可读提示（EBUSY=卡未挂载 / ENOENT=不存在，两者动作不同） */
  function fileErr(e) {
    return (MP.Files && typeof MP.Files.describeError === 'function')
      ? MP.Files.describeError(e) : U.errText(e);
  }

  function showProgress(p) {
    if (!p) return;
    var names = { upload: '上传', download: '下载', 'test-write': '写入 PC2P4.TXT', 'test-done': '写入完成' };
    var label = names[p.phase] || p.phase;
    var cur = 0, total = 0;
    if (p.phase === 'upload' || p.phase === 'test-write') { cur = p.written || 0; total = p.total || 0; }
    else if (p.phase === 'download') { cur = p.loaded || 0; total = p.total || 0; }
    else if (p.phase === 'test-done') { cur = p.written || 0; total = p.total || 0; }
    var pct = total > 0 ? Math.min(100, Math.round(cur * 100 / total)) : 0;
    setText('files-status', label + ' ' + (p.path || '') + '：' + U.fmtBytes(cur) +
      (total ? (' / ' + U.fmtBytes(total) + '（' + pct + '%）') : ''));
  }

  function renderCrumbs() {
    var box = el('crumbs');
    if (!box) return;
    var doc = root.document;
    while (box.firstChild) box.removeChild(box.firstChild);
    var segs = U.splitPath(S.cwd);

    var rootBtn = doc.createElement('button');
    rootBtn.className = 'crumb';
    rootBtn.textContent = '/';
    rootBtn.title = '根目录';
    rootBtn.addEventListener('click', function () { UI.refreshDir('/'); });
    box.appendChild(rootBtn);

    var acc = '';
    for (var i = 0; i < segs.length; i++) {
      var sep = doc.createElement('span');
      sep.className = 'sep';
      sep.textContent = '/';
      box.appendChild(sep);
      acc += '/' + segs[i];
      var target = acc;
      if (i === segs.length - 1) {
        var cur = doc.createElement('span');
        cur.className = 'cur';
        cur.textContent = segs[i];
        box.appendChild(cur);
      } else {
        var b = doc.createElement('button');
        b.className = 'crumb';
        b.textContent = segs[i];
        b.title = target;
        b.addEventListener('click', (function (t) {
          return function () { UI.refreshDir(t); };
        })(target));
        box.appendChild(b);
      }
    }
  }

  function renderFileList(entries) {
    var body = el('file-list');
    if (!body) return;
    var doc = root.document;
    while (body.firstChild) body.removeChild(body.firstChild);

    if (!entries || !entries.length) {
      var tr0 = doc.createElement('tr');
      var td0 = doc.createElement('td');
      td0.className = 'empty';
      td0.colSpan = 4;
      td0.textContent = S.connected ? (S.linkUp ? '（空目录）' : '链路未 up，暂无内容') : '未连接';
      tr0.appendChild(td0);
      body.appendChild(tr0);
      return;
    }

    for (var i = 0; i < entries.length; i++) {
      (function (entry) {
        var tr = doc.createElement('tr');
        if (S.selected === entry.name) tr.className = 'selected';

        var tdName = doc.createElement('td');
        tdName.className = 'name';
        var ico = doc.createElement('span');
        ico.className = 'ico' + (entry.isDir ? '' : ' file');
        ico.textContent = entry.isDir ? '▸' : '·';
        tdName.appendChild(ico);
        var nm = doc.createElement('span');
        nm.className = 'nm';
        nm.textContent = entry.name;
        tdName.appendChild(nm);
        tdName.title = entry.name;
        tdName.addEventListener('click', function () {
          if (entry.isDir) UI.refreshDir(U.joinPath(S.cwd, entry.name));
          else UI.selectEntry(entry.name);
        });
        tr.appendChild(tdName);

        var tdSize = doc.createElement('td');
        tdSize.className = 'num';
        tdSize.textContent = entry.isDir ? '—' : U.fmtBytes(entry.size);
        tr.appendChild(tdSize);

        var tdType = doc.createElement('td');
        tdType.textContent = entry.isDir ? '目录' : '文件';
        tr.appendChild(tdType);

        var tdAct = doc.createElement('td');
        tdAct.className = 'act';
        if (entry.isDir) {
          var bIn = doc.createElement('button');
          bIn.className = 'btn tiny';
          bIn.textContent = '进入';
          bIn.addEventListener('click', function () { UI.refreshDir(U.joinPath(S.cwd, entry.name)); });
          tdAct.appendChild(bIn);
        } else {
          var bDl = doc.createElement('button');
          bDl.className = 'btn tiny';
          bDl.textContent = '下载';
          bDl.addEventListener('click', function () { UI.downloadEntry(entry.name); });
          tdAct.appendChild(bDl);
        }
        var bSel = doc.createElement('button');
        bSel.className = 'btn tiny';
        bSel.textContent = '选择';
        bSel.addEventListener('click', function () { UI.selectEntry(entry.name); });
        tdAct.appendChild(bSel);
        tr.appendChild(tdAct);

        body.appendChild(tr);
      })(entries[i]);
    }
  }

  UI.selectEntry = function (name) {
    S.selected = (S.selected === name) ? null : name;
    renderFileList(S.entries);
    UI.updateButtons();
  };

  UI.refreshDir = async function (path) {
    var target = U.normalizePath(path || '/');
    if (!S.files || !S.connected) { UI.toast('warn', '还没连接设备'); return; }
    if (!S.linkUp) { setText('files-status', '链路未 up（等 HELLO）'); return; }
    if (S.filesBusy) return;

    S.filesBusy = true;
    addClass('files-status', 'busy');
    UI.updateButtons();
    setText('files-status', '正在列目录 ' + target + ' …');
    try {
      var r = await S.files.listAll(target);
      S.cwd = target;
      S.entries = MP.Files.sortEntries(MP.Files.filterNavigational(r.entries));
      S.selected = null;
      renderCrumbs();
      renderFileList(S.entries);
      setText('files-status', '共 ' + S.entries.length + ' 项 · 翻了 ' + r.pages + ' 页' +
        (r.offsetIgnored ? '（设备未按 offset 分页，已兼容）' : '') +
        (r.truncatedTail ? (' · 尾部残片 ' + r.truncatedTail + ' B 已丢弃') : ''));
      UI.log('info', '列目录 ' + target + '：' + S.entries.length + ' 项（' + r.pages + ' 页）');
    } catch (e) {
      S.entries = [];
      renderFileList([]);
      setText('files-status', '列目录失败：' + fileErr(e));
      UI.log('err', '列目录 ' + target + ' 失败：' + fileErr(e));
      UI.toast('err', '列目录 ' + target + ' 失败：' + fileErr(e));
    } finally {
      S.filesBusy = false;
      removeClass('files-status', 'busy');
      UI.updateButtons();
    }
  };

  UI.downloadEntry = async function (name) {
    if (!S.files) return;
    var path = U.joinPath(S.cwd, name);
    try {
      UI.log('info', '开始下载 ' + path + ' …');
      var r = await S.files.download(path);
      UI.log('ok', '已下载 ' + r.name + '（' + U.fmtBytes(r.size) + '）→ 浏览器下载目录');
      UI.toast('ok', '已下载 ' + r.name + '（' + U.fmtBytes(r.size) + '）');
      setText('files-status', '已下载 ' + r.name + '（' + U.fmtBytes(r.size) + '）');
    } catch (e) {
      UI.log('err', '下载失败：' + fileErr(e));
      UI.toast('err', '下载 ' + path + ' 失败：' + fileErr(e));
      setText('files-status', '下载失败：' + fileErr(e));
    }
  };

  function askText(msg, def) {
    if (typeof root.prompt === 'function') return root.prompt(msg, def);
    UI.toast('warn', '当前环境没有 prompt()，无法输入名称');
    return null;
  }

  function askConfirm(msg) {
    if (typeof root.confirm === 'function') return !!root.confirm(msg);
    return true;
  }

  UI.newDir = async function () {
    if (!S.files || !S.linkUp) return;
    var name = askText('新目录名（不含 / 或 \\）', 'NEWDIR');
    if (name === null) return;
    name = String(name).trim();
    if (!U.isValidName(name)) { UI.toast('warn', '目录名不合法：' + name); return; }
    var target = U.joinPath(S.cwd, name);
    try {
      await S.files.mkdir(target);
      UI.log('ok', '已创建目录 ' + target);
      UI.toast('ok', '已创建目录 ' + target);
      await UI.refreshDir(S.cwd);
    } catch (e) {
      UI.log('err', '新建目录失败：' + fileErr(e));
      UI.toast('err', '新建目录 ' + target + ' 失败：' + fileErr(e));
    }
  };

  UI.renameEntry = async function () {
    if (!S.files || !S.linkUp || !S.selected) return;
    var oldName = S.selected;
    var newName = askText('把「' + oldName + '」重命名为：', oldName);
    if (newName === null) return;
    newName = String(newName).trim();
    if (!U.isValidName(newName)) { UI.toast('warn', '名字不合法：' + newName); return; }
    if (newName === oldName) return;
    var from = U.joinPath(S.cwd, oldName);
    var to = U.joinPath(S.cwd, newName);
    try {
      await S.files.rename(from, to);
      UI.log('ok', '已重命名 ' + from + ' → ' + to);
      UI.toast('ok', '已重命名 ' + oldName + ' → ' + newName);
      await UI.refreshDir(S.cwd);
    } catch (e) {
      UI.log('err', '重命名失败：' + fileErr(e));
      UI.toast('err', '重命名失败：' + fileErr(e));
    }
  };

  UI.deleteEntry = async function () {
    if (!S.files || !S.linkUp || !S.selected) return;
    var name = S.selected;
    var path = U.joinPath(S.cwd, name);
    if (!askConfirm('确定删除「' + path + '」吗？此操作不可撤销。')) return;
    try {
      await S.files.remove(path);
      UI.log('ok', '已删除 ' + path);
      UI.toast('ok', '已删除 ' + path);
      S.selected = null;
      await UI.refreshDir(S.cwd);
    } catch (e) {
      UI.log('err', '删除失败：' + fileErr(e));
      UI.toast('err', '删除 ' + path + ' 失败：' + fileErr(e));
    }
  };

  UI.uploadFiles = async function (fileList) {
    if (!S.files || !S.linkUp || !fileList || !fileList.length) return;
    for (var i = 0; i < fileList.length; i++) {
      var f = fileList[i];
      try {
        UI.log('info', '上传 ' + f.name + '（' + U.fmtBytes(f.size || 0) + '）→ ' + S.cwd + ' …');
        var r = await S.files.upload(S.cwd, f);
        UI.log('ok', '已上传 ' + r.path + '（' + U.fmtBytes(r.size) + '）');
        UI.toast('ok', '已上传 ' + r.name + '（' + U.fmtBytes(r.size) + '）');
      } catch (e) {
        UI.log('err', '上传 ' + (f && f.name) + ' 失败：' + fileErr(e));
        UI.toast('err', '上传 ' + (f && f.name) + ' 失败：' + fileErr(e));
      }
    }
    await UI.refreshDir(S.cwd);
  };

  UI.writeTestFile = async function () {
    if (!S.files || !S.linkUp) return;
    UI.log('info', '一键写入 /PC2P4.TXT（带时间戳），随后回读逐字节校验 …');
    setText('files-status', '正在写入 /PC2P4.TXT …');
    try {
      var r = await S.files.writeTestFile('/');
      UI.log(r.verified ? 'ok' : 'warn', '写入 /PC2P4.TXT 完成：写入 ' + r.bytes + ' B（WRITE_RSP 累计 ' + r.written +
        ' B），回读 ' + r.readBack + ' B，逐字节校验=' + (r.verified ? '通过' : '不一致'));
      UI.toast(r.verified ? 'ok' : 'err', r.verified
        ? ('PC→P4 共享写入成功：/PC2P4.TXT（' + r.bytes + ' B，标记 ' + r.marker + '），已回读校验。' +
           '现在可在 PC 的 MTP 卷里打开同一文件核对。')
        : ('写入后回读不一致：期望 ' + r.bytes + ' B，回读 ' + r.readBack + ' B。请看日志里的 FILE 事务。'));
      setText('files-status', '已写入 /PC2P4.TXT（' + r.bytes + ' B）' + (r.verified ? '，回读校验通过' : '，回读校验失败'));
      await UI.refreshDir('/');
      S.selected = 'PC2P4.TXT';
      renderFileList(S.entries);
      UI.updateButtons();
    } catch (e) {
      UI.log('err', '写入 /PC2P4.TXT 失败：' + fileErr(e));
      UI.toast('err', '写入 /PC2P4.TXT 失败：' + fileErr(e));
      setText('files-status', '写入失败：' + fileErr(e));
    }
  };

  UI.ping = async function () {
    if (!S.link) return;
    setText('ping-result', 'PING …');
    try {
      var rtt = await S.link.ping(2000);
      setText('ping-result', 'PONG ' + rtt + ' ms');
      UI.log('ok', 'PING/PONG 往返 ' + rtt + ' ms');
    } catch (e) {
      setText('ping-result', '失败：' + U.errText(e));
      UI.log('warn', 'PING 失败：' + U.errText(e));
    }
  };

  /* ================================================================ 初始化 */

  function on(id, ev, fn) {
    var e = el(id);
    if (e && e.addEventListener) e.addEventListener(ev, fn);
  }

  UI.init = function () {
    if (S.initialised) return;
    S.initialised = true;

    var doc = root.document;
    if (doc && doc.addEventListener) {
      doc.addEventListener('visibilitychange', function () { if (!doc.hidden) UI.refreshStats(); });
    }

    on('btn-connect', 'click', function () { UI.connect(); });
    on('btn-disconnect', 'click', function () { UI.disconnect(false); });
    on('btn-log-clear', 'click', function () {
      var box = el('log');
      if (box) { while (box.firstChild) box.removeChild(box.firstChild); }
      S.logLines = 0;
      UI.log('info', '（日志已清屏）');
    });
    on('btn-msg-clear', 'click', function () {
      var box = el('msg-list');
      if (box) { while (box.firstChild) box.removeChild(box.firstChild); }
    });
    on('btn-ping', 'click', function () { UI.ping(); });
    on('btn-files-refresh', 'click', function () { UI.refreshDir(S.cwd); });
    on('btn-files-up', 'click', function () { UI.refreshDir(U.parentPath(S.cwd)); });
    on('btn-newdir', 'click', function () { UI.newDir(); });
    on('btn-rename', 'click', function () { UI.renameEntry(); });
    on('btn-delete', 'click', function () { UI.deleteEntry(); });
    on('btn-write-test', 'click', function () { UI.writeTestFile(); });
    on('btn-upload', 'click', function () { var i = el('file-input'); if (i && i.click) i.click(); });
    on('file-input', 'change', function (ev) {
      var input = (ev && ev.target) || el('file-input');
      var files = (input && input.files) ? input.files : null;
      if (files && files.length) UI.uploadFiles(files);
      if (input) input.value = '';
    });

    // 环境自检
    S.usbAvailable = !!(root.navigator && root.navigator.usb);
    if (!S.usbAvailable) {
      setPill('pill-usb', 'err', 'USB 不可用');
      setDisabled('btn-connect', true);
      UI.log('err', 'navigator.usb 不存在：请用 Chrome / Edge 打开，且页面必须在 file:// 或 http://localhost 等安全上下文。');
      UI.toast('err', '此浏览器不支持 WebUSB（navigator.usb 不存在）。请用 Chrome / Edge 打开本页；' +
        'Firefox / Safari 不支持。');
    } else {
      UI.log('info', 'WebUSB 可用（navigator.usb 存在）');
      if (root.navigator.usb.addEventListener) {
        root.navigator.usb.addEventListener('connect', function (ev) {
          var d = ev && ev.device;
          if (d && d.vendorId === MP.Usb.VID && d.productId === MP.Usb.PID) {
            UI.log('info', '检测到 MeidaP4 接入（VID 303A / PID 4103）：点「连接设备」授权。');
            UI.toast('info', '检测到 MeidaP4 接入，点「连接设备」开始。');
          }
        });
      }
      try {
        root.navigator.usb.getDevices().then(function (list) {
          if (list && list.length) {
            UI.log('info', '已授权过 ' + list.length + ' 个 USB 设备（file:// 下授权可能不持久，重连需重新授权）');
          }
        }, function () { /* 忽略：某些版本没有 getDevices */ });
      } catch (ignore) { /* 忽略 */ }
    }

    if (root.location && root.location.protocol === 'file:') {
      UI.log('info', '页面以 file:// 打开（安全上下文，WebUSB 可用；但 file:// 下不能加载外部脚本/模块）');
    }

    // 画布占位
    addClass('canvas-wrap', 'empty');
    renderCrumbs();
    renderFileList([]);
    UI.updateButtons();
    UI.log('info', 'MeidaP4 控制台就绪。设备：VID 0x303A / PID 0x4103；' +
      'IF1=视频(EP 0x83 IN)、IF2=控制(EP 0x84 IN / 0x04 OUT)');

    // 定时刷新统计（400 ms；只读快照，不参与协议）
    S.statsTimer = root.setInterval(UI.refreshStats, STATS_PERIOD_MS);
    UI.refreshStats();
  };

  /** 自检/调试用：把 UI 内部状态清成"未连接"（不碰 DOM 事件） */
  UI._resetForTest = function () {
    S.connected = false;
    S.linkUp = false;
    S.caps = 0;
    S.usb = null;
    S.link = null;
    S.video = null;
    S.files = null;
    S.entries = [];
    S.selected = null;
    S.cwd = '/';
  };

})(typeof window !== 'undefined' ? window : globalThis);
