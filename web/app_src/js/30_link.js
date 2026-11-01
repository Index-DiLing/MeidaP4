// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · 链路会话层（I0 协议的主机侧状态机）
//
// 本模块是"薄胶水"：把冻结实现 MP.WU.LinkSession（= web/proto.js 的逐字节镜像）
// 接到 WebUSB 传输层，并把 FILE 通道交给 MP.FILE.FileClient（= web/file.js 镜像）。
// **协议逻辑一律不在这里重写**：
//   * HELLO / HELLO_ACK（含 seq 门豁免、引导帧无状态）→ LinkSession
//   * 可靠窗口 W=8 / 200 ms 重传 / 累计 ACK / 5 次判死 → ReliableChannel（LinkSession 内含）
//   * 3 s 无有效帧 ⇒ link_down → LinkSession.tick()
//   * ZLP 计数 → LinkSession.onZeroLenRead()
//   * FILE 的 xid 配对 / 超时 / 分块读写 → FileClient
//
// 主机侧额外只做三件事：
//   1. 周期 tick（50 ms）驱动重传与 link_down 判定；
//   2. link_up 后每 5 s 发一个 PING（链路活性探测），只有窗口有空位时才发；
//   3. 把事件翻译给 UI（HELLO / TELEM / LOG / 链路状态 / 通道异常）。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};
  var WU = MP.WU;
  var U = MP.util;

  var TICK_MS = 50;
  var PING_IDLE_MS = 5000;       // link_up 后每 5 s 一次 PING（协议 §5.4 的心跳节奏只约束设备侧）
  var PING_TIMEOUT_MS = 2000;

  function Link(usb, hooks) {
    var self = this;
    this.usb = usb;
    this.hooks = hooks || {};

    this.session = new WU.LinkSession({
      send: function (bytes) { return usb.sendBytes(bytes); },
      now: function () { return Date.now(); }
    });
    this.fc = new MP.FILE.FileClient({ session: this.session, timeoutMs: 2500 });

    this._timer = null;
    this._lastPingMs = 0;
    this._pingWaiters = [];
    this._pingSeq = 0;
    this._lastState = this.session.state;

    // ---- 通道消息派发 ------------------------------------------------
    this.session.on('message', function (m) {
      if (m.channel === WU.CHANNEL.FILE) {
        self.fc.onMessage(m.payload);
      } else if (m.channel === WU.CHANNEL.TELEM) {
        var t = WU.parseTelem(m.payload);
        if (t) self.hooks.onTelem && self.hooks.onTelem(t);
      } else if (m.channel === WU.CHANNEL.LOG) {
        var text = WU.bytesToUtf8(m.payload);
        if (text) self.hooks.onLogLine && self.hooks.onLogLine(text.replace(/\r?\n$/, ''));
      } else if (m.channel === WU.CHANNEL.VIDEO) {
        // 视频走 IF1 独立端点；本会话只处理 IF2。这里出现说明端点接错，记一笔。
        self.hooks.onUnexpected && self.hooks.onUnexpected('控制端点上收到 VIDEO 消息（' + m.payload.length + ' B）');
      }
    });

    // ---- CTRL 层事件 --------------------------------------------------
    this.session.on('ctrl', function (c) {
      if (c.opcode === WU.OP.HELLO) return;      // 由 'hello' 事件处理
      if (c.opcode === WU.OP.HELLO_ACK) return;
      if (c.opcode === WU.OP.PING) { self.hooks.onCtrl && self.hooks.onCtrl('收到设备 PING'); return; }
      if (c.opcode === WU.OP.ACK) return;        // 每帧都可能有 ACK，别刷屏
      if (c.opcode === WU.OP.CREDIT) {
        self.hooks.onCtrl && self.hooks.onCtrl('收到 CREDIT：通道 ' + c.arg + ' 额度 ' +
          WU.rd16(c.body || [], 0) + ' B（暂不据此调整窗口）');
        return;
      }
      self.hooks.onCtrl && self.hooks.onCtrl('收到 ' + (WU.OP_NAMES[c.opcode] || ('opcode 0x' + c.opcode.toString(16))) +
        '（status=' + c.status + ' arg=' + c.arg + '）');
    });

    this.session.on('hello', function (h) {
      // 设备是序号主导方：LinkSession 已经复位本侧所有 TX seq / RX 期望 / 拼接态。
      // FILE 的 xid 空间是"每会话"的，也必须一起复位（未完成请求会被 EABORT 拒绝）。
      self.fc.reset('hello');
      if (self.hooks.onHello) {
        self.hooks.onHello({ version: h.version, caps: h.caps, raw: h });
      }
    });

    this.session.on('helloAck', function () {
      if (self.hooks.onHelloAck) self.hooks.onHelloAck();
    });

    this.session.on('pong', function () {
      var waiters = self._pingWaiters;
      self._pingWaiters = [];
      for (var i = 0; i < waiters.length; i++) {
        try { waiters[i](); } catch (ignore) { /* 回调异常不影响协议 */ }
      }
      if (self.hooks.onPong) self.hooks.onPong();
    });

    this.session.on('link', function (s) {
      self._lastState = s.state;
      if (s.state !== 'up') {
        // 链路掉了：未完成的文件事务没有意义了（序号将重置）
        self.fc.reset('link_' + s.state);
        self._failPingWaiters('链路断开');
      }
      if (self.hooks.onLink) self.hooks.onLink(s);
    });

    this.session.on('gap', function (g) {
      if (self.hooks.onGap) {
        self.hooks.onGap('通道 ' + (WU.CHANNEL_NAMES[g.channel] || g.channel) +
          ' 出现 seq 空洞：收到 ' + g.got + '，期望 ' + g.expected + '（等重传，累计 ACK 停在期望值）');
      }
    });
  }

  /**
   * 连接后**主动**发几次 HELLO_ACK（直到 link_up）。
   *
   * ⚠️ 2026-10-31 真机修正（"无画面 / 几百帧后卡死"的主机侧一半）：
   * 设备可能是"上一台主机会话留下的 link_up"状态（TX seq 已经涨到几百），而本页
   * 只在**收到 HELLO** 时才回 HELLO_ACK 并复位序号。设备一直不判 link_down 就永远不发
   * HELLO ⇒ 本页 rx_expect 停在 1，设备发 569/570… ⇒ 全是"seq 空洞" ⇒ 死锁。
   * 主动 HELLO_ACK 会强制设备走一次 §6.3 的 link_up 归零，双方序号立刻对齐。
   * 只在 state !== 'up' 时发；一旦 up 就停（重复发会把正在进行的可靠事务重置掉）。
   */
  Link.prototype.helloHandshake = function () {
    var self = this;
    if (this._hsTimer) return;
    var tries = 0;
    this._hsTimer = setInterval(function () {
      if (self.session.state === 'up' || ++tries > 60) {
        clearInterval(self._hsTimer);
        self._hsTimer = null;
        return;
      }
      try { self.session.sendHelloAck(); } catch (ignore) { /* 端点还没好，下一轮再试 */ }
    }, 250);
  };
  Link.prototype._failPingWaiters = function (why) {    var waiters = this._pingWaiters;
    this._pingWaiters = [];
    for (var i = 0; i < waiters.length; i++) {
      try { waiters[i](new Error(why)); } catch (ignore) { /* 忽略 */ }
    }
  };

  Link.prototype.start = function () {
    var self = this;
    if (this._timer) return;
    this._lastPingMs = Date.now();
    this._timer = root.setInterval(function () { self.tick(); }, TICK_MS);
  };

  Link.prototype.stop = function () {
    if (this._timer) { root.clearInterval(this._timer); this._timer = null; }
    this._failPingWaiters('会话已停止');
  };

  /** 手动 tick（自检里用假时钟驱动，不依赖 setInterval） */
  Link.prototype.tick = function (nowMs) {
    var t = (nowMs === undefined) ? Date.now() : nowMs;
    this.session.tick(t);
    if (this.session.state !== 'up') return;
    if (t - this._lastPingMs < PING_IDLE_MS) return;
    // 窗口有空位才发（避免把 8 帧窗口塞满 PING、把文件事务饿死）
    if (this.session.ctrl.space <= 0) return;
    this._lastPingMs = t;
    try { this.session.sendPing(); } catch (e) { /* 发送失败下一次再试 */ }
  };

  /** 把控制端点读到的原始字节喂给会话（由 UI 挂到 usb.handlers.control） */
  Link.prototype.feedControl = function (bytes) {
    this.session.onBytes(bytes);
  };

  /** 传输层报告 0 字节读（ZLP）时调用：只计数，不推进状态（协议 §4.1） */
  Link.prototype.onZeroLenRead = function () {
    return this.session.onZeroLenRead();
  };

  /**
   * 手动 PING 并测 RTT。
   * @returns {Promise<number>} RTT 毫秒
   */
  Link.prototype.ping = function (timeoutMs) {
    var self = this;
    var ms = timeoutMs || PING_TIMEOUT_MS;
    return new Promise(function (resolve, reject) {
      if (self.session.state !== 'up') { reject(new Error('链路未建立（还没收到 HELLO）')); return; }
      if (!self.session.sendPing()) {
        reject(new Error('PING 没能发出：可靠窗口已满（有未确认帧）——先等文件传输告一段落'));
        return;
      }
      var t0 = Date.now();
      var done = false;
      var timer = root.setTimeout(function () {
        if (done) return;
        done = true;
        var idx = self._pingWaiters.indexOf(cb);
        if (idx >= 0) self._pingWaiters.splice(idx, 1);
        reject(new Error('PING 超过 ' + ms + ' ms 没有 PONG（设备侧心跳/ACK 可能没实现）'));
      }, ms);
      function cb(err) {
        if (done) return;
        done = true;
        root.clearTimeout(timer);
        if (err) reject(err);
        else resolve(Date.now() - t0);
      }
      self._pingWaiters.push(cb);
    });
  };

  /** 便捷发送（UI 的调试按钮用） */
  Link.prototype.sendPing = function () { return this.session.sendPing(); };
  Link.prototype.sendCtrl = function (opcode, opts) { return this.session.sendCtrl(opcode, opts); };
  Link.prototype.sendFile = function (op, opts) { return this.session.sendFile(op, opts); };

  Link.prototype.snapshot = function () {
    var s = this.session.stats;
    var ctrl = this.session.ctrl.stats;
    var file = this.session.file.stats;
    var fc = this.fc.stats;
    return {
      state: s.state,
      linkUp: s.linkUp,
      caps: this.session.caps,
      deviceVersion: this.session.deviceVersion,
      zeroLenReads: s.zeroLenReads,
      frames: s.frames,
      crcErrors: s.crcErrors,
      resyncBytes: s.resyncBytes,
      badHeader: s.badHeader,
      linkUps: s.linkUps,
      linkDowns: s.linkDowns,
      rxMessages: s.rxMessages,
      txMessages: s.txMessages,
      ctrl: { unacked: ctrl.unacked, sent: ctrl.sent, retransmits: ctrl.retransmits, acked: ctrl.acked, nextSeq: ctrl.nextSeq },
      file: { unacked: file.unacked, sent: file.sent, retransmits: file.retransmits, acked: file.acked, nextSeq: file.nextSeq },
      fileClient: {
        requests: fc.requests, responses: fc.responses, timeouts: fc.timeouts,
        unmatched: fc.unmatchedResponses, malformed: fc.malformed, pending: this.fc.pending
      }
    };
  };

  MP.Link = Link;
})(typeof window !== 'undefined' ? window : globalThis);
