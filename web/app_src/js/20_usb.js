// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · WebUSB 传输层
//
// 只负责"把字节搬进搬出"，不解析协议（协议在 MP.WU / MP.Link 里）。
//
// 冻结的物理分配（docs/contracts/frozen-allocation.md §1/§2）：
//   IF0  MTP（不 claim）
//   IF1  Vendor 视频 —— 只有 IN，EP3 = 0x83（HS bulk 512）
//   IF2  Vendor 控制 —— IN EP4 = 0x84 / OUT EP4 = 0x04（HS bulk 512）
//   IF3/4 NCM（不 claim）
//
// 三条必须遵守的现场经验（docs/protocol.md §4.1 + docs/probe-report.md）：
//   1. 0 字节 transferIn（ZLP）**合法**，计数后继续读，绝不当中止条件；
//   2. 两个接口各自独立读、各自独立帧解码（跨端点拼接字节是错的）；
//   3. OUT 必须**串行**写（并发 transferOut 的顺序没有保证）。
//
// 本模块不假设"一定能连上"：claim IF1 失败会降级成"仅控制模式"，
// 并把可读原因（很可能是固件没给 IF1 绑 WinUSB/WCID）交给 UI。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};
  var U = MP.util;

  var VID = 0x303A;
  var PID = 0x4103;
  var IF_VIDEO = 1;
  var IF_CTRL = 2;

  var VIDEO_READ_LEN = 8192;     // 视频端点单次读：够大以减少 WebUSB 往返
  var CTRL_READ_LEN = 4096;      // 控制端点（CTRL/FILE/LOG/TELEM）
  var OUT_TIMEOUT_MS = 5000;     // transferOut 兜底：绝不无限挂起

  function usbUnavailable() {
    return !(root.navigator && root.navigator.usb);
  }

  /** 可读的 claim 失败判读（中文） */
  function claimHint(err, ifaceNo, role) {
    var name = (err && err.name) || '';
    var what = ifaceNo === IF_VIDEO ? 'IF1（视频，EP 0x83 IN）' : 'IF2（控制，EP 0x84 IN / 0x04 OUT）';
    var who = role === '视频' ? '视频通道' : '控制/文件通道';
    if (name === 'SecurityError' || name === 'InvalidStateError') {
      return '接口 ' + what + ' 的 claim 被拒绝：Chrome 只能 claim「驱动已绑定为 WinUSB」的接口。\n' +
        '  · ' + (ifaceNo === IF_VIDEO
          ? 'IF1 需要固件里带 IF1 的 MS OS 2.0 Function Subset + WINUSB 兼容 ID（WCID）。请确认已烧写含 IF1 WCID 的固件。'
          : 'IF2 的 WCID 应已随固件下发；若仍失败，看设备管理器里 MI_02 的驱动是不是 winusb.sys。') + '\n' +
        '  · 烧写/换描述符后必须**关闭本页并拔插一次 USB**，让 Windows 重新读取 MS OS 描述符（bcdDevice 变了才会刷新）。\n' +
        '  · 关掉其它正在占用该设备的 Chrome 标签页（同一接口只能被一个页面 claim），必要时在 chrome://device-log 看拒绝原因。\n' +
        '  · 兜底：用 Zadig 给「MeidaP4 WebUSB」对应接口装 WinUSB（开发期可接受）。\n' +
        '  ⇒ ' + who + '不可用。';
    }
    if (name === 'NotFoundError') {
      return '接口 ' + what + ' 在当前配置里不存在：本页按冻结分配表找 IF1/IF2。\n' +
        '  · 若设备是探测期固件（PID 0x4001/0x4002），接口号完全不同；本页只认 PID 0x4103 的完整版复合设备。\n' +
        '  · 也可能是 selectConfiguration 没生效（先「断开」再重连）。';
    }
    if (name === 'NetworkError') {
      return '接口 ' + what + ' 的 claim 期间设备断开/复位了。请重新插拔后重试。';
    }
    return '接口 ' + what + ' 的 claim 失败（' + (name || '未知错误') + '）。看上方日志里的 err.name / err.message 原文，' +
      '或查 chrome://device-log。';
  }

  function withTimeout(promise, ms, what) {
    return new Promise(function (resolve, reject) {
      var done = false;
      var timer = root.setTimeout(function () {
        if (done) return;
        done = true;
        var e = new Error(what + ' 超时（' + ms + ' ms）');
        e.name = 'ETIMEDOUT';
        reject(e);
      }, ms);
      promise.then(function (v) {
        if (done) return;
        done = true;
        root.clearTimeout(timer);
        resolve(v);
      }, function (e) {
        if (done) return;
        done = true;
        root.clearTimeout(timer);
        reject(e);
      });
    });
  }

  /* ==================================================================== */

  function Usb(opts) {
    opts = opts || {};
    var self = this;

    this.device = null;
    this.claimed = { video: false, ctrl: false };
    this.eps = { videoIn: null, ctrlIn: null, ctrlOut: null };

    /** 上层挂进来的数据回调：{control(bytes), video(bytes)} */
    this.handlers = { control: null, video: null };

    this.zeroLenReads = { control: 0, video: 0 };
    this.reads = { control: 0, video: 0 };
    this.rxBytes = { control: 0, video: 0 };
    this.txBytes = 0;
    this.stalls = 0;
    this.readErrors = 0;
    this.handlerErrors = 0;
    this.outErrors = 0;
    this.outTimeouts = 0;
    this.outDropped = 0;
    this.outQueued = 0;

    this.opened = false;
    this.disconnected = false;
    this._running = false;
    this._outQueue = [];
    this._outBusy = false;
    this._readTasks = [];
    this._onUsbDisconnect = null;

    this.onLog = opts.log || function () {};
    this.onDisconnect = opts.onDisconnect || null;
    this.onZlp = opts.onZlp || null;
    this.onError = opts.onError || null;
  }

  Usb.VID = VID;
  Usb.PID = PID;
  Usb.IF_VIDEO = IF_VIDEO;
  Usb.IF_CTRL = IF_CTRL;

  Usb.prototype._log = function (level, text) { this.onLog(level, text); };

  Usb.prototype._fail = function (e, hint) {
    if (hint && e && !e.hint) {
      try { e.hint = hint; } catch (ignore) { /* 某些 DOMException 的字段只读 */ }
    }
    if (this.onError) this.onError(e);
    return e;
  };

  /* --------------------------------------------------------------- 连接 */

  /**
   * 弹窗选设备 → open → 选配置 → claim IF1(视频) / IF2(控制) → 找端点。
   *
   * @returns {Promise<{video:{ok:boolean,error:Error|null,endpoint:number|null},
   *                    ctrl:{ok:boolean,epIn:number,epOut:number}}>}
   *          IF2 claim 失败会 **throw**（没有控制通道，页面无意义）；
   *          IF1 失败只降级（文件/日志可用，画面不可用）。
   */
  Usb.prototype.connect = async function () {
    var self = this;
    if (usbUnavailable()) {
      throw this._fail(new Error('此浏览器不支持 WebUSB（navigator.usb 不存在）。' +
        '请用 Chrome / Edge，并把页面放在 file:// 或 http://localhost 等安全上下文下打开。'));
    }
    if (this.device && this.opened) {
      throw this._fail(new Error('设备已经连接（先点「断开」再重连）。'));
    }

    this._log('info', 'requestDevice(filters=[{vendorId:0x303A, productId:0x4103}]) …');
    var dev;
    try {
      dev = await root.navigator.usb.requestDevice({ filters: [{ vendorId: VID, productId: PID }] });
    } catch (e) {
      if (e && e.name === 'NotFoundError') {
        throw this._fail(new Error('请求设备被取消或没有匹配设备：请插稳 P4，确认设备管理器里出现 ' +
          'VID_303A&PID_4103 的复合设备（不是探测期的 0x4001/0x4002）。'), e.hint);
      }
      throw this._fail(e);
    }
    if (!dev) throw this._fail(new Error('requestDevice 返回空（用户取消或没有匹配设备）。'));

    this.device = dev;
    this.disconnected = false;
    this._log('ok', '已选中设备：' + (dev.productName || '(无产品名)') +
      '  VID=0x' + (dev.vendorId || 0).toString(16).toUpperCase() +
      '  PID=0x' + (dev.productId || 0).toString(16).toUpperCase() +
      '  序列号=' + (dev.serialNumber || '(无)'));

    try {
      if (!dev.opened) await dev.open();
    } catch (e) {
      throw this._fail(new Error('设备 open() 失败：' + U.errText(e) +
        '\n  · 可能被其它页面/程序占用（关闭其它 Chrome 标签页或 Zadig 之类的工具）。'), e.hint);
    }
    this.opened = true;
    this._log('ok', 'device.open() 成功');

    // 选择配置（复合设备只有一个配置；configuration 为 null 时必须显式选）
    try {
      if (!dev.configuration) {
        var cfgs = dev.configurations || [];
        if (!cfgs.length) {
          throw this._fail(new Error('设备没有可用配置（configuration 为 null 且 configurations 为空）。' +
            '多半是刚复位/枚举未完成，拔插后重试。'));
        }
        await dev.selectConfiguration(cfgs[0].configurationValue);
        this._log('ok', 'selectConfiguration(' + cfgs[0].configurationValue + ') 成功');
      } else {
        this._log('info', '设备已处于配置 ' + dev.configuration.configurationValue);
      }
    } catch (e) {
      if (e && e.hint) throw e;   // 上面自定义的
      throw this._fail(new Error('selectConfiguration 失败：' + U.errText(e)), e.hint);
    }

    this._log('info', this.describeInterfaces());

    // 监听拔出（顺便在 close() 里摘掉）
    if (root.navigator.usb && root.navigator.usb.addEventListener) {
      this._onUsbDisconnect = function (ev) {
        if (ev && ev.device && ev.device === self.device) self._handleGone('设备拔出（disconnect 事件）');
      };
      root.navigator.usb.addEventListener('disconnect', this._onUsbDisconnect);
    }

    var result = {
      video: { ok: false, error: null, endpoint: null },
      ctrl: { ok: false, epIn: null, epOut: null }
    };

    // ---- IF2（控制）：没有它页面就没意义，失败直接抛 ----
    try {
      await this._claim(IF_CTRL);
      this.claimed.ctrl = true;
      var cIn = this.findEndpoint(IF_CTRL, 'in', 'bulk');
      var cOut = this.findEndpoint(IF_CTRL, 'out', 'bulk');
      if (!cIn || !cOut) {
        throw this._fail(new Error('IF2 缺少 bulk IN 或 OUT 端点（冻结分配要求 0x84 / 0x04）。' +
          '当前配置：' + this.describeInterfaces()));
      }
      this.eps.ctrlIn = cIn.endpointNumber;
      this.eps.ctrlOut = cOut.endpointNumber;
      result.ctrl = { ok: true, epIn: cIn.endpointNumber, epOut: cOut.endpointNumber };
      this._log('ok', 'claimInterface(2) 成功：控制 IN=0x' + (0x80 | cIn.endpointNumber).toString(16) +
        '，控制 OUT=0x' + cOut.endpointNumber.toString(16) +
        '（wMaxPacketSize=' + (cIn.packetSize || '?') + '）');
    } catch (e) {
      this._fail(e, claimHint(e, IF_CTRL, '控制'));
      throw e;
    }

    // ---- IF1（视频）：可选，失败降级 ----
    try {
      await this._claim(IF_VIDEO);
      this.claimed.video = true;
      var vIn = this.findEndpoint(IF_VIDEO, 'in', 'bulk');
      if (!vIn) {
        throw this._fail(new Error('IF1 缺少 bulk IN 端点（冻结分配要求 0x83）。'));
      }
      this.eps.videoIn = vIn.endpointNumber;
      result.video = { ok: true, error: null, endpoint: vIn.endpointNumber };
      this._log('ok', 'claimInterface(1) 成功：视频 IN=0x' + (0x80 | vIn.endpointNumber).toString(16) +
        '（wMaxPacketSize=' + (vIn.packetSize || '?') + '）');
    } catch (e) {
      result.video = { ok: false, error: e, endpoint: null };
      this._fail(e, claimHint(e, IF_VIDEO, '视频'));
      this._log('warn', 'IF1（视频）claim 失败，已降级为「仅控制模式」：文件/日志/遥测可用，画面不可用。');
      this._log('warn', '原因：' + U.errText(e));
      // 视频接口留空；为了安全，把可能半成功的 claim 释放掉
      try { await this._release(IF_VIDEO); } catch (ignore) { /* 没 claim 上就算了 */ }
      this.claimed.video = false;
    }

    return result;
  };

  Usb.prototype._claim = async function (ifaceNumber) {
    if (!this.device || !this.device.configuration) {
      throw new Error('还没 selectConfiguration，无法 claimInterface(' + ifaceNumber + ')');
    }
    var itf = null;
    var list = this.device.configuration.interfaces || [];
    for (var i = 0; i < list.length; i++) if (list[i].interfaceNumber === ifaceNumber) itf = list[i];
    if (!itf) {
      var e = new Error('当前配置里没有接口 IF' + ifaceNumber);
      e.name = 'NotFoundError';
      throw e;
    }
    if (itf.claimed) {
      this._log('warn', 'IF' + ifaceNumber + ' 已被本页 claim，先 release 再 claim');
      try { await this.device.releaseInterface(ifaceNumber); } catch (ignore) { /* 继续尝试 */ }
    }
    try {
      await this.device.claimInterface(ifaceNumber);
    } catch (err) {
      throw err;
    }
  };

  Usb.prototype._release = async function (ifaceNumber) {
    if (!this.device || !this.opened) return;
    try { await this.device.releaseInterface(ifaceNumber); }
    catch (e) { this._log('warn', 'releaseInterface(' + ifaceNumber + ') 失败：' + U.errText(e)); }
  };

  /** 在已激活的 alternate setting 里找一个端点 */
  Usb.prototype.findEndpoint = function (ifaceNumber, direction, type) {
    if (!this.device || !this.device.configuration) return null;
    var list = this.device.configuration.interfaces || [];
    var itf = null;
    for (var i = 0; i < list.length; i++) if (list[i].interfaceNumber === ifaceNumber) itf = list[i];
    if (!itf) return null;
    var alt = itf.alternate || (itf.alternates && itf.alternates.length ? itf.alternates[0] : null);
    var eps = (alt && alt.endpoints) || [];
    var typed = null, anyDir = null;
    for (var k = 0; k < eps.length; k++) {
      if (eps[k].direction !== direction) continue;
      if (!anyDir) anyDir = eps[k];
      if (!type || eps[k].type === type) { typed = eps[k]; break; }
    }
    return typed || anyDir;
  };

  /** 端点/接口清单（日志用；不假设一定齐） */
  Usb.prototype.describeInterfaces = function () {
    var d = this.device;
    if (!d || !d.configuration) return '接口清单：(未配置)';
    var lines = ['配置 ' + d.configuration.configurationValue + ' 的接口：'];
    var list = d.configuration.interfaces || [];
    for (var i = 0; i < list.length; i++) {
      var itf = list[i];
      var alt = itf.alternate || (itf.alternates && itf.alternates[0]) || null;
      var eps = (alt && alt.endpoints) || [];
      var desc = [];
      for (var k = 0; k < eps.length; k++) {
        var ep = eps[k];
        desc.push((ep.direction === 'in' ? 'IN 0x' + (0x80 | ep.endpointNumber).toString(16)
          : 'OUT 0x' + ep.endpointNumber.toString(16)) + ' ' + ep.type + ' ' + (ep.packetSize || '?') + 'B');
      }
      lines.push('  IF' + itf.interfaceNumber + (itf.claimed ? ' [已 claim]' : '') +
        '  类=0x' + Number((alt && alt.interfaceClass) || 0).toString(16) +
        '  端点: ' + (desc.length ? desc.join(' / ') : '(无)'));
    }
    return lines.join('\n');
  };

  /* --------------------------------------------------------------- 读 */

  Usb.prototype.startReads = function () {
    if (this._running) return;
    this._running = true;
    if (this.claimed.ctrl && this.eps.ctrlIn !== null) {
      this._readTasks.push(this._readLoop('control', this.eps.ctrlIn, CTRL_READ_LEN));
    }
    if (this.claimed.video && this.eps.videoIn !== null) {
      this._readTasks.push(this._readLoop('video', this.eps.videoIn, VIDEO_READ_LEN));
    }
    this._log('info', '已启动持续读循环：' + this._readTasks.length + ' 个端点（0 字节读 = 合法 ZLP，会计数后继续）');
  };

  Usb.prototype._readLoop = async function (kind, epNumber, readLen) {
    var self = this;
    var consecutiveErrors = 0;
    var consecutiveStalls = 0;

    while (this._running && this.device && this.opened) {
      var res;
      try {
        res = await this.device.transferIn(epNumber, readLen);
      } catch (e) {
        if (!this._running) break;
        this.readErrors++;
        consecutiveErrors++;
        // 拔出 / 复位：读会以 NetworkError 拒绝，这只意味着"会话结束"，不是帧错误
        if (e && (e.name === 'NetworkError' || e.name === 'NotFoundError')) {
          this._handleGone(kind + ' IN 读取中止：' + U.errText(e));
          break;
        }
        this._log('warn', kind + ' IN 读取异常（第 ' + consecutiveErrors + ' 次）：' + U.errText(e));
        if (consecutiveErrors >= 5) {
          this._handleGone(kind + ' IN 连续 ' + consecutiveErrors + ' 次读取异常，停止读循环');
          break;
        }
        await U.sleep(Math.min(50 * consecutiveErrors, 500));
        continue;
      }
      consecutiveErrors = 0;
      if (!this._running) break;
      this.reads[kind]++;

      if (!res || res.status !== 'ok') {
        // 'stall' 可能是端点被 halt（例如设备侧异常）。尝试 clearHalt 后继续；不做致命判决。
        this.stalls++;
        consecutiveStalls++;
        this._log('warn', kind + ' IN 传输 status=' + (res && res.status) + '（累计 ' + this.stalls + ' 次）');
        try { await this.device.clearHalt('in', epNumber); } catch (ignore) { /* 尽力而为 */ }
        if (consecutiveStalls >= 8) {
          this._handleGone(kind + ' IN 连续 ' + consecutiveStalls + ' 次 STALL，停止读循环');
          break;
        }
        await U.sleep(60);
        continue;
      }
      consecutiveStalls = 0;

      var data = res.data;
      if (!data || data.byteLength === 0) {
        // ★ 0 字节是合法 ZLP（docs/protocol.md §4.1）：计数、通知、继续读，绝不中止。
        this.zeroLenReads[kind]++;
        if (this.onZlp) {
          try { this.onZlp(kind, this.zeroLenReads[kind]); } catch (ignore) { /* 回调不得影响读循环 */ }
        }
        continue;
      }

      var view = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      this.rxBytes[kind] += view.length;
      var handler = (kind === 'video') ? this.handlers.video : this.handlers.control;
      if (typeof handler === 'function') {
        try {
          handler(view);
        } catch (e) {
          // 上层解析异常绝不能打死读循环（否则一个坏帧就让画面永久冻住）
          this.handlerErrors++;
          if (this.handlerErrors <= 5 || this.handlerErrors % 100 === 0) {
            this._log('err', kind + ' 数据回调异常（第 ' + this.handlerErrors + ' 次）：' + U.errText(e));
          }
        }
      }
    }
  };

  /* --------------------------------------------------------------- 写 */

  /**
   * 把一帧（已编码的 12 B 头 + payload）排进 OUT 队列。
   * 返回 true = 已入队（协议层据此认为"发出去了"，重传由 200 ms 超时兜底）。
   */
  Usb.prototype.sendBytes = function (bytes) {
    if (!this.device || !this.opened || !this.claimed.ctrl || this.eps.ctrlOut === null) return false;
    if (!bytes || !bytes.length) return false;
    if (this._outQueue.length >= 512) {
      this._outQueue.shift();          // 极端拥塞：丢最老的，保证控制帧不无限堆积
      this.outDropped++;
    }
    this._outQueue.push(bytes);
    this.outQueued++;
    this._pumpOut();
    return true;
  };

  Usb.prototype._pumpOut = function () {
    var self = this;
    if (this._outBusy) return;
    this._outBusy = true;
    (async function () {
      while (self._outQueue.length) {
        if (!self.device || !self.opened) break;
        var chunk = self._outQueue.shift();
        try {
          var res = await withTimeout(
            self.device.transferOut(self.eps.ctrlOut, chunk), OUT_TIMEOUT_MS, 'transferOut');
          if (!res || res.status !== 'ok') {
            self.outErrors++;
            if (self.outErrors <= 5) self._log('warn', 'transferOut status=' + (res && res.status) + '（帧未送达，等超时重传）');
          } else {
            self.txBytes += (res.bytesWritten || chunk.length);
          }
        } catch (e) {
          if (e && e.name === 'ETIMEDOUT') {
            self.outTimeouts++;
            if (self.outTimeouts <= 5) self._log('warn', 'transferOut 超时（设备 NAK？该帧由可靠通道超时重传）');
          } else {
            self.outErrors++;
            if (e && (e.name === 'NetworkError' || e.name === 'NotFoundError')) {
              self._handleGone('OUT 写入中止：' + U.errText(e));
              break;
            }
            if (self.outErrors <= 5) self._log('warn', 'transferOut 异常：' + U.errText(e));
          }
        }
      }
      self._outBusy = false;
    })();
  };

  /* --------------------------------------------------------------- 收尾 */

  Usb.prototype._handleGone = function (reason) {
    if (this.disconnected) return;
    this.disconnected = true;
    this._running = false;
    this._outQueue.length = 0;
    this._log('warn', reason);
    if (this.onDisconnect) {
      try { this.onDisconnect(reason); } catch (ignore) { /* 回调异常不得影响收尾 */ }
    }
  };

  Usb.prototype.close = async function () {
    this._running = false;
    this._outQueue.length = 0;
    if (this._onUsbDisconnect && root.navigator.usb && root.navigator.usb.removeEventListener) {
      try { root.navigator.usb.removeEventListener('disconnect', this._onUsbDisconnect); } catch (ignore) { /* 忽略 */ }
      this._onUsbDisconnect = null;
    }
    if (this.device && this.opened) {
      if (this.claimed.video) { await this._release(IF_VIDEO); this.claimed.video = false; }
      if (this.claimed.ctrl) { await this._release(IF_CTRL); this.claimed.ctrl = false; }
      try { await this.device.close(); } catch (e) { this._log('warn', 'device.close() 异常：' + U.errText(e)); }
      this._log('info', '已释放接口并关闭设备');
    }
    this.opened = false;
    this.device = null;
    this.eps = { videoIn: null, ctrlIn: null, ctrlOut: null };
    this.handlers = { control: null, video: null };
  };

  /** 诊断快照（UI 每几百毫秒取一次） */
  Usb.prototype.snapshot = function () {
    return {
      opened: this.opened,
      disconnected: this.disconnected,
      videoClaimed: this.claimed.video,
      ctrlClaimed: this.claimed.ctrl,
      eps: { videoIn: this.eps.videoIn, ctrlIn: this.eps.ctrlIn, ctrlOut: this.eps.ctrlOut },
      zeroLenReads: { control: this.zeroLenReads.control, video: this.zeroLenReads.video },
      reads: { control: this.reads.control, video: this.reads.video },
      rxBytes: { control: this.rxBytes.control, video: this.rxBytes.video },
      txBytes: this.txBytes,
      stalls: this.stalls,
      readErrors: this.readErrors,
      handlerErrors: this.handlerErrors,
      outErrors: this.outErrors,
      outTimeouts: this.outTimeouts,
      outDropped: this.outDropped,
      outPending: this._outQueue.length
    };
  };

  MP.Usb = Usb;
})(typeof window !== 'undefined' ? window : globalThis);
