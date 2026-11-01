// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · 视频通道（VIDEO / 不可靠，走 IF1 EP 0x83）
//
// 设备侧约定（docs/protocol.md §5.2 + 视频工作流补充）：
//   一条 VIDEO **消息**的 payload = 16 B 视频头 + 编码数据（每消息 ≤ 1008 B 数据）
//      0   u8  fmt         0=MJPEG 1=RGB565 2=H264
//      1   u8  flags       bit0 = 关键帧
//      2   u16 width
//      4   u16 height
//      6   u16 fps_num
//      8   u16 fps_den
//     10   u32 frame_seq   同一帧的所有分片共用同一个 frame_seq
//     14   u16 chunk_index 从 0 递增
//   一**帧** = 多条消息（同一 frame_seq、chunk_index 从 0 连续递增）。
//
// 因此本模块做两件事：
//   ① 消息级：FrameDecoder（12 B 帧头 + CRC） + Assembler（MORE 分片）
//   ② 帧级：按 frame_seq/chunk_index 重组；MJPEG 用 JPEG 结束符 FF D9 判定"这一帧收全了"，
//      收到新 frame_seq 时若上一帧没收全 ⇒ 记丢帧并丢弃（**绝不请求重传**，§5.2 冻结策略）。
//
// 渲染：`createImageBitmap` + canvas 绘制；**到达即覆盖，不排队**（只保留最新一帧，
// 解码慢时宁可丢中间帧也不让延迟累积）。画布按容器自适应（ResizeObserver + DPR）。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};
  var WU = MP.WU;
  var U = MP.util;

  var JPEG_SOI = [0xFF, 0xD8];
  var JPEG_EOI = [0xFF, 0xD9];
  var MAX_FRAME_BYTES = 8 * 1024 * 1024;   // 单帧上限（防被坏头骗到无限缓存）
  var FPS_WINDOW_MS = 2000;                // 统计窗口
  var FPS_DECODE_GRACE = 0.35;             // 解码滞后补偿 ≤35%，超了就把显示上限压在 1.35×

  function Video(opts) {
    opts = opts || {};
    var self = this;

    this.canvas = opts.canvas || null;
    this.ctx = (this.canvas && this.canvas.getContext) ? this.canvas.getContext('2d') : null;
    this.onLog = opts.onLog || function () {};
    this.onStats = opts.onStats || null;
    this.onFirstFrame = opts.onFirstFrame || null;

    this.decoder = new WU.FrameDecoder();
    this.asm = new WU.Assembler(WU.CHANNEL.VIDEO, MAX_FRAME_BYTES);

    this.cur = null;              // 正在拼的帧
    this.lastCompletedSeq = null; // 上一帧完成的 frame_seq
    this._skipSeq = null;         // 已知残缺的 frame_seq（等它过去）
    this._pending = null;         // 待解码帧（最新覆盖）
    this._rendering = false;
    this._hasFrame = false;
    this._warnedFmt = {};
    this._lastWarnMs = 0;
    this._lastDrawLog = 0;

    this.stats = {
      messages: 0,
      messageBytes: 0,
      foreignFrames: 0,
      badMessages: 0,
      framesStarted: 0,
      framesComplete: 0,
      framesDecoded: 0,
      droppedFrames: 0,
      droppedPartial: 0,
      dupChunks: 0,
      gapChunks: 0,
      decodeErrors: 0,
      totalBytes: 0,
      decodeMs: 0,
      lastDrawMs: 0
    };

    this._win = [];        // [{t, n}] 完成帧的时间/字节，用于 fps / 码率
    this.lastHeader = null;

    // 画布自适应
    this._ro = null;
    if (this.canvas && root.ResizeObserver) {
      try {
        this._ro = new root.ResizeObserver(function () { self.redrawLast(); });
        var wrap = (this.canvas.parentElement || this.canvas);
        this._ro.observe(wrap);
      } catch (ignore) { /* 老环境没有就算了，绘制时也会读尺寸 */ }
    }
  }

  /* ------------------------------------------------------------ 消息级入口 */

  /** 喂入 IF1 IN 上拿到的原始字节（可能包含多个帧/半帧；0 字节由传输层拦掉） */
  Video.prototype.feed = function (bytes) {
    var frames = this.decoder.push(bytes);
    for (var i = 0; i < frames.length; i++) {
      var f = frames[i];
      if (f.channel !== WU.CHANNEL.VIDEO) { this.stats.foreignFrames++; continue; }
      var msg = this.asm.push(f);
      if (msg === null) continue;
      try {
        this._onMessage(msg);
      } catch (e) {
        this.stats.badMessages++;
        this.warn('视频消息解析异常：' + U.errText(e));
      }
    }
  };

  Video.prototype.warn = function (text) {
    var t = Date.now();
    if (t - this._lastWarnMs < 1000) return;   // 限流：避免坏流把日志刷爆
    this._lastWarnMs = t;
    this.onLog('warn', text);
  };

  /* ------------------------------------------------------------ 帧级重组 */

  Video.prototype._onMessage = function (payload) {
    if (payload.length < WU.VIDEO_HDR_LEN) {
      this.stats.badMessages++;
      return;
    }
    var fmt = payload[0];
    var kf = payload[1];
    var w = WU.rd16(payload, 2);
    var h = WU.rd16(payload, 4);
    var fn = WU.rd16(payload, 6);
    var fd = WU.rd16(payload, 8);
    var seq = WU.rd32(payload, 10);
    var ci = WU.rd16(payload, 14);
    var data = payload.subarray(WU.VIDEO_HDR_LEN);

    this.stats.messages++;
    this.stats.messageBytes += data.length;

    if (!this._warnedFmt[fmt] && fmt !== 0) {
      this._warnedFmt[fmt] = true;
      this.warn('收到非 MJPEG 视频（fmt=' + fmt + '）：本页只解码 MJPEG（fmt=0），该路只统计不显示。');
    }

    // ① 新 frame_seq ⇒ 上一帧结束了
    if (this.cur && seq !== this.cur.frameSeq) this._finish(false);

    // ② 还没在拼：只接受 chunk_index = 0 的分片
    if (!this.cur) {
      if (this._skipSeq === seq && ci !== 0) return;   // 这一帧已知残缺，等它过去
      if (ci !== 0) {
        if (this._skipSeq !== seq) {
          this._skipSeq = seq;
          this.stats.droppedPartial++;
        }
        return;
      }
      this._skipSeq = null;
      this.cur = {
        frameSeq: seq, fmt: fmt, keyframe: kf, w: w, h: h,
        fpsNum: fn, fpsDen: fd, nextChunk: 0, parts: [], len: 0
      };
      this.stats.framesStarted++;
    }

    var cur = this.cur;
    if (!cur || cur.frameSeq !== seq) return;

    // ③ 分片顺序：只接受"正好下一片"；重复片忽略；跳片 ⇒ 整帧作废（等下一个 frame_seq）
    if (ci < cur.nextChunk) { this.stats.dupChunks++; return; }
    if (ci > cur.nextChunk) {
      this.stats.gapChunks++;
      this._skipSeq = seq;
      this._abortPartial();
      return;
    }

    var nextLen = cur.len + data.length;
    if (nextLen > MAX_FRAME_BYTES) {
      this._skipSeq = seq;
      this._abortPartial();
      this.warn('单帧累计超过 ' + MAX_FRAME_BYTES + ' B，判定为坏流并丢弃该帧');
      return;
    }
    if (data.length) cur.parts.push(data);
    cur.len = nextLen;
    cur.nextChunk++;

    // ④ 完整性判定：MJPEG 以 FF D9 收尾；其它格式只能等下一个 frame_seq
    if (fmt === 0 && cur.len >= 4 && data.length >= 2 && U.endsWith(data, JPEG_EOI)) {
      this._finish(true);
    }
  };

  /** 结束当前帧；complete=false 表示没收全（丢帧，不重传） */
  Video.prototype._finish = function (complete) {
    var cur = this.cur;
    if (!cur) return;
    this.cur = null;

    if (!complete) {
      this.stats.droppedPartial++;
      return;
    }

    var bytes = U.concatBytes(cur.parts);
    if (!bytes.length) { this.stats.droppedPartial++; return; }
    // MJPEG 必须真的以 EOI 收尾，否则不能交给解码器（会得到 "broken image"）
    if (cur.fmt === 0 && !U.endsWith(bytes, JPEG_EOI)) {
      this.stats.droppedPartial++;
      return;
    }

    var now = Date.now();
    this.stats.framesComplete++;
    this.stats.totalBytes += bytes.length;

    if (this.lastCompletedSeq !== null) {
      var gap = (cur.frameSeq - this.lastCompletedSeq - 1) >>> 0;
      // 合理范围才计丢帧（避免乱序/换号把计数爆掉）
      if (gap > 0 && gap < 1000000) this.stats.droppedFrames += gap;
    }
    this.lastCompletedSeq = cur.frameSeq;
    this.lastHeader = {
      fmt: cur.fmt, keyframe: cur.keyframe, w: cur.w, h: cur.h,
      fpsNum: cur.fpsNum, fpsDen: cur.fpsDen, frameSeq: cur.frameSeq, bytes: bytes.length
    };
    this._win.push({ t: now, n: bytes.length });
    this._pruneWindow(now);

    if (!this._hasFrame) {
      this._hasFrame = true;
      if (this.onFirstFrame) {
        try { this.onFirstFrame(this.lastHeader); } catch (ignore) { /* 回调异常不影响解码 */ }
      }
    }

    this._render(bytes, cur);
  };

  Video.prototype._abortPartial = function () {
    if (!this.cur) return;
    this.cur = null;
    this.stats.droppedPartial++;
  };

  Video.prototype._pruneWindow = function (now) {
    var cut = now - FPS_WINDOW_MS;
    var w = this._win;
    var drop = 0;
    while (drop < w.length && w[drop].t < cut) drop++;
    if (drop) w.splice(0, drop);
    U.trimArray(w, 4096);
  };

  /* ------------------------------------------------------------ 解码渲染 */

  /** 最新帧覆盖式渲染：解码慢时丢弃中间的帧，绝不让延迟累积 */
  Video.prototype._render = function (bytes, meta) {
    this._pending = { bytes: bytes, meta: meta };
    if (this._rendering) return;
    var self = this;
    this._rendering = true;
    (async function () {
      while (self._pending) {
        var f = self._pending;
        self._pending = null;
        if (!root.createImageBitmap) {
          self.stats.decodeErrors++;
          self.warn('此环境没有 createImageBitmap（需要 Chrome/Edge 88+）。');
          break;
        }
        var t0 = (root.performance && root.performance.now) ? root.performance.now() : Date.now();
        try {
          var blob = new root.Blob([f.bytes], { type: 'image/jpeg' });
          var bmp = await root.createImageBitmap(blob);
          self._draw(bmp, f.meta);
          if (bmp.close) bmp.close();
          self.stats.framesDecoded++;
          var t1 = (root.performance && root.performance.now) ? root.performance.now() : Date.now();
          self.stats.decodeMs = Math.round(t1 - t0);
        } catch (e) {
          self.stats.decodeErrors++;
          if (self.stats.decodeErrors <= 3 || self.stats.decodeErrors % 50 === 0) {
            self.warn('JPEG 解码失败（第 ' + self.stats.decodeErrors + ' 次，' + f.bytes.length + ' B）：' + U.errText(e));
          }
        }
      }
      self._rendering = false;
    })();
  };

  Video.prototype._draw = function (bmp, meta) {
    if (!this.ctx || !this.canvas) return;
    var t0 = (root.performance && root.performance.now) ? root.performance.now() : Date.now();
    var dpr = root.devicePixelRatio || 1;
    var cw = this.canvas.clientWidth || this.canvas.width || 640;
    var ch = this.canvas.clientHeight || this.canvas.height || 360;
    var pw = Math.max(1, Math.round(cw * dpr));
    var ph = Math.max(1, Math.round(ch * dpr));
    if (this.canvas.width !== pw) this.canvas.width = pw;
    if (this.canvas.height !== ph) this.canvas.height = ph;

    var vw = bmp.width || (meta && meta.w) || 0;
    var vh = bmp.height || (meta && meta.h) || 0;
    var ctx = this.ctx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = '#05070c';
    ctx.fillRect(0, 0, pw, ph);
    if (vw > 0 && vh > 0) {
      var s = Math.min(pw / vw, ph / vh);
      var dw = Math.max(1, Math.round(vw * s));
      var dh = Math.max(1, Math.round(vh * s));
      var dx = Math.round((pw - dw) / 2);
      var dy = Math.round((ph - dh) / 2);
      ctx.imageSmoothingEnabled = true;
      ctx.drawImage(bmp, dx, dy, dw, dh);
    }
    this._lastBitmap = { bmp: bmp, meta: meta, version: (this._lastBitmap ? this._lastBitmap.version + 1 : 1) };
    var t1 = (root.performance && root.performance.now) ? root.performance.now() : Date.now();
    this.stats.lastDrawMs = Math.round(t1 - t0);
  };

  /** 容器尺寸变化时用缓存的重绘（不重新解码） */
  Video.prototype.redrawLast = function () {
    if (!this._lastBitmap) return;
    this._draw(this._lastBitmap.bmp, this._lastBitmap.meta);
  };

  /** 清空统计（重连时用） */
  Video.prototype.reset = function () {
    this.resetStream();
    var s = this.stats;
    s.messages = 0; s.messageBytes = 0; s.foreignFrames = 0; s.badMessages = 0;
    s.framesStarted = 0; s.framesComplete = 0; s.framesDecoded = 0;
    s.droppedFrames = 0; s.droppedPartial = 0; s.dupChunks = 0; s.gapChunks = 0;
    s.decodeErrors = 0; s.totalBytes = 0; s.decodeMs = 0; s.lastDrawMs = 0;
    this._win = [];
    this.lastHeader = null;
    this._hasFrame = false;
    this.lastCompletedSeq = null;
  };

  /**
   * 只清"流状态"（解码器 / 拼接中 / 待解码帧），**保留累计统计**。
   * 用于收到 HELLO（设备会重置自己的 TX 序号，之前的半帧字节已经不可信）时：
   * 最多丢一帧，但不会把用户看到的累计数字清零。
   */
  Video.prototype.resetStream = function () {
    this.decoder.reset();
    this.asm.reset();
    this.cur = null;
    this._skipSeq = null;
    this._pending = null;
    this.lastCompletedSeq = null;
  };

  /** 给 UI 的统计快照（含 fps / 码率，按时间窗口算） */
  Video.prototype.snapshot = function () {
    var now = Date.now();
    this._pruneWindow(now);
    var n = this._win.length;
    var bytes = 0;
    for (var i = 0; i < this._win.length; i++) bytes += this._win[i].n;
    var span = FPS_WINDOW_MS / 1000;
    var fps = n / span;
    var bytesPerSec = bytes / span;
    var h = this.lastHeader;
    return {
      hasFrame: this._hasFrame,
      framesDecoded: this.stats.framesDecoded,
      framesComplete: this.stats.framesComplete,
      droppedFrames: this.stats.droppedFrames,
      droppedPartial: this.stats.droppedPartial,
      decodeErrors: this.stats.decodeErrors,
      messages: this.stats.messages,
      avgBytes: this.stats.framesComplete ? Math.round(this.stats.totalBytes / this.stats.framesComplete) : 0,
      totalBytes: this.stats.totalBytes,
      fps: fps,
      bytesPerSec: bytesPerSec,
      decodeMs: this.stats.decodeMs,
      lastDrawMs: this.stats.lastDrawMs,
      header: h,
      decoderStats: this.decoder.stats,
      decoderDiag: this.decoder.diagnostics,
      asmStats: { msgOk: this.asm.stats.msgOk, msgAborted: this.asm.stats.msgAborted }
    };
  };

  MP.Video = Video;
})(typeof window !== 'undefined' ? window : globalThis);
