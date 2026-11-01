// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · TF 卡文件操作（FILE 通道 / 可靠）
//
// FILE 通道的消息格式归 I0（冻结）：
//   8 B 头 = {u8 op, u8 status, u16 xid, u32 offset} + body
//   body 语义见 web/file.js 顶部表格（本页与设备侧对齐后的口径）：
//     LIST_REQ : path\0                    LIST_RSP : N × {u32 size, u8 is_dir, name[256]}（每条 261 B）
//     OPEN_REQ : u8 mode + path\0          OPEN_RSP : u32 handle, u32 size, u8 is_dir（≥9 B，设备回 9 B）
//     READ_REQ : u32 handle, u32 length    READ_RSP : 数据（8 B 头里的 offset = 读位置；0 字节 = EOF）
//     WRITE_REQ: u32 handle + 数据         WRITE_RSP: u32 written
//     CLOSE_REQ: u32 handle                CLOSE_RSP: 空
//     DELETE/MKDIR/RENAME 没有独立 RSP：设备**原样回同一个 op**，按 xid 配对
//
// ⚠️ 设备侧的 **LIST 分页约定**（Lead 转达、与实现对齐）：
//   LIST_REQ 的 `offset` 字段 = **起始条目序号**（不是字节偏移）；
//   单帧最多 3 条（3×261 = 783 B body，8+783 = 791 B ≤ 1024）；
//   主机按 offset 递增翻页，直到某页 0 条或 < 3 条为止。
//   本模块的 listAll() 就是这么翻的，并且对"设备忽略 offset、一次给全"的实现做了兼容。
//
// 本模块只做"高层语义 + 翻页 + 浏览器侧下载/上传"，协议可靠性全部交给
// MP.Link 里的 LinkSession / FileClient（xid 配对、超时、重传）。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};
  var WU = MP.WU;
  var F = MP.FILE;
  var U = MP.util;

  var LIST_PAGE_ENTRIES = 3;      // 设备侧单帧最多 3 条（约定，见文件头）
  var MAX_LIST_PAGES = 1024;      // 防御：最多翻 1024 页（≈3000 条）
  var MAX_LIST_ENTRIES = 20000;   // 防御：单目录条目上限

  /** 路径 → NUL 结尾的 UTF-8 字节（与 web/file.js 内部 pathBytes 同语义） */
  function pathBytes(path) {
    var b = WU.utf8ToBytes(String(path === undefined ? '/' : path));
    for (var i = 0; i < b.length; i++) {
      if (b[i] === 0) throw new F.FileError('EINVAL', '路径里不允许出现 NUL 字节');
    }
    var out = new Uint8Array(b.length + 1);
    out.set(b, 0);
    return out;
  }

  function statusOf(msg) {
    return F.STATUS_NAME[msg.status] || ('0x' + Number(msg.status).toString(16));
  }

  function checkStatus(msg, what) {
    if (msg.status !== WU.STATUS.OK) {
      throw new F.FileError('ESTATUS',
        what + ' 返回 ' + statusOf(msg) + '（status=0x' + Number(msg.status).toString(16) + '）', msg.status);
    }
    return msg;
  }

  /* ================================================================== */

  function Files(link, hooks) {
    this.link = link;
    this.fc = link.fc;
    this.hooks = hooks || {};
    this.stats = {
      pages: 0, entries: 0, offsetIgnored: false, truncatedTail: 0,
      uploads: 0, downloads: 0, bytesUp: 0, bytesDown: 0
    };
  }

  Files.prototype._progress = function (info) {
    if (this.hooks.onProgress) {
      try { this.hooks.onProgress(info); } catch (ignore) { /* 回调异常不影响协议 */ }
    }
  };

  /* ------------------------------------------------------------ 列目录 */

  /** 请求一页（最多 3 条）；offset = 起始条目序号 */
  Files.prototype.listPage = async function (path, offset) {
    var msg = await this.fc.request(WU.FILE_OP.LIST_REQ, {
      offset: offset >>> 0,
      body: pathBytes(path)
    });
    checkStatus(msg, 'LIST_REQ ' + path);
    if (msg.op !== WU.FILE_OP.LIST_RSP && msg.op !== WU.FILE_OP.LIST_REQ) {
      throw new F.FileError('EPROTO', 'LIST_REQ 期望 LIST_RSP(' + WU.FILE_OP.LIST_RSP + ')，实际 op=' + msg.op);
    }
    var r = F.decodeListEntries(msg.body);
    return { entries: r.entries, truncatedTail: r.truncatedTail, offset: offset, bytes: msg.body.length };
  };

  /**
   * 列一个目录的全部条目（按设备分页约定翻页聚合）。
   *
   * 终止条件（任一）：
   *   · 本页 0 条；
   *   · 本页 < 3 条（最后一页）；
   *   · 本页 > 3 条（设备没按分页来、一次给全 —— 兼容旧语义，记 offsetIgnored）；
   *   · 设备忽略 offset（返回与上一页同一批）—— 防御性退出，绝不无限循环。
   *
   * @returns {Promise<{entries:Array, pages:number, truncatedTail:number, offsetIgnored:boolean}>}
   */
  Files.prototype.listAll = async function (path) {
    var p = U.normalizePath(path);
    var all = [];
    var offset = 0;
    var pages = 0;
    var truncatedTail = 0;
    var offsetIgnored = false;
    var prevFirst = null;

    for (;;) {
      var page = await this.listPage(p, offset);
      pages++;
      truncatedTail += page.truncatedTail;
      var ents = page.entries;

      if (!ents.length) break;

      // 防御：设备忽略 offset（同一批反复返回）
      if (offset > 0 && prevFirst !== null && ents[0].name === prevFirst) {
        offsetIgnored = true;
        this.stats.offsetIgnored = true;
        if (this.hooks.onNotice) {
          this.hooks.onNotice('设备似乎没有按 offset 翻页（第 ' + pages + ' 页与上一页首条相同），已停止翻页。' +
            '若目录条目不全，请报 Lead：LIST_REQ 的 offset 需要按"起始条目序号"实现。');
        }
        break;
      }
      prevFirst = ents[0].name;

      for (var i = 0; i < ents.length; i++) all.push(ents[i]);

      if (all.length > MAX_LIST_ENTRIES) {
        throw new F.FileError('EIO', '目录 ' + p + ' 条目超过 ' + MAX_LIST_ENTRIES + ' 条，已停止翻页（防跑飞）');
      }
      if (ents.length > LIST_PAGE_ENTRIES) {
        // 设备一次给全（未分页）：已经拿到全部，直接结束
        offsetIgnored = true;
        this.stats.offsetIgnored = true;
        break;
      }
      if (ents.length < LIST_PAGE_ENTRIES) break;   // 最后一页

      offset += ents.length;
      if (pages >= MAX_LIST_PAGES) {
        throw new F.FileError('EIO', '目录 ' + p + ' 超过 ' + MAX_LIST_PAGES + ' 页，已停止翻页（防跑飞）');
      }
    }

    this.stats.pages += pages;
    this.stats.entries = all.length;
    this.stats.truncatedTail = truncatedTail;
    return { entries: all, pages: pages, truncatedTail: truncatedTail, offsetIgnored: offsetIgnored };
  };

  /* ------------------------------------------------------------ 目录操作 */

  Files.prototype.mkdir = async function (path) {
    var msg = await this.fc.request(WU.FILE_OP.MKDIR, { body: pathBytes(path) });
    checkStatus(msg, 'MKDIR ' + path);
    if (msg.op !== WU.FILE_OP.MKDIR && msg.op !== (0x80 | WU.FILE_OP.MKDIR)) {
      throw new F.FileError('EPROTO', 'MKDIR 响应 op=' + msg.op + '（约定 = 原样回同一个 op）');
    }
    return true;
  };

  Files.prototype.rename = async function (from, to) {
    // body = src\0 dst\0（用 web/file.js 导出的权威编码，避免自己拼）
    var body = F.encodeRenameBody(from, to);
    var msg = await this.fc.request(WU.FILE_OP.RENAME, { body: body });
    checkStatus(msg, 'RENAME ' + from + ' → ' + to);
    if (msg.op !== WU.FILE_OP.RENAME && msg.op !== (0x80 | WU.FILE_OP.RENAME)) {
      throw new F.FileError('EPROTO', 'RENAME 响应 op=' + msg.op + '（约定 = 原样回同一个 op）');
    }
    return true;
  };

  Files.prototype.remove = async function (path) {
    var msg = await this.fc.request(WU.FILE_OP.DELETE, { body: pathBytes(path) });
    checkStatus(msg, 'DELETE ' + path);
    if (msg.op !== WU.FILE_OP.DELETE && msg.op !== (0x80 | WU.FILE_OP.DELETE)) {
      throw new F.FileError('EPROTO', 'DELETE 响应 op=' + msg.op + '（约定 = 原样回同一个 op）');
    }
    return true;
  };

  /* ------------------------------------------------------------ 上传下载 */

  /** 读整个文件（OPEN→READ 循环→CLOSE），返回 Uint8Array */
  Files.prototype.readFile = async function (path, opts) {
    var self = this;
    var r = await this.fc.readFile(path, {
      chunkSize: (opts && opts.chunkSize) || undefined,
      onProgress: function (p) {
        self._progress({ phase: 'download', path: path, loaded: p.loaded, total: p.total });
      }
    });
    this.stats.downloads++;
    this.stats.bytesDown += r.bytes.length;
    return r;
  };

  /** 浏览器侧另存为（file:// 下合法：Blob + <a download>，不涉及网络/外部资源） */
  Files.prototype.saveBytes = function (name, bytes, mime) {
    var doc = root.document;
    if (!root.URL || !root.URL.createObjectURL || !doc) {
      throw new Error('当前环境不支持 Blob 下载（缺 URL.createObjectURL / document）');
    }
    var blob = new root.Blob([bytes], { type: mime || 'application/octet-stream' });
    var url = root.URL.createObjectURL(blob);
    var a = doc.createElement('a');
    a.href = url;
    a.download = name || 'download.bin';
    a.rel = 'noopener';
    if (doc.body && doc.body.appendChild) doc.body.appendChild(a);
    a.click();
    root.setTimeout(function () {
      try { if (a.remove) a.remove(); else if (a.parentNode) a.parentNode.removeChild(a); } catch (ignore) { /* 忽略 */ }
      try { root.URL.revokeObjectURL(url); } catch (ignore) { /* 忽略 */ }
    }, 10000);
    return url;
  };

  /** 下载到浏览器（文件名 = 路径 basename） */
  Files.prototype.download = async function (path, opts) {
    var r = await this.readFile(path, opts);
    var name = U.baseName(path) || 'download.bin';
    this.saveBytes(name, r.bytes);
    return { name: name, size: r.bytes.length, bytes: r.bytes };
  };

  /** 把浏览器 File 对象的内容读成 Uint8Array */
  function readBrowserFile(file) {
    if (file && typeof file.arrayBuffer === 'function') {
      return file.arrayBuffer().then(function (ab) { return new Uint8Array(ab); });
    }
    return new Promise(function (resolve, reject) {
      if (!root.FileReader) { reject(new Error('此环境没有 FileReader，无法读取本地文件')); return; }
      var fr = new root.FileReader();
      fr.onload = function () { resolve(new Uint8Array(fr.result)); };
      fr.onerror = function () { reject(new Error('读取本地文件失败：' + (file && file.name))); };
      fr.readAsArrayBuffer(file);
    });
  }

  /** 上传一个浏览器 File 到设备目录下同名文件 */
  Files.prototype.upload = async function (dir, file) {
    var self = this;
    var name = (file && file.name) || 'upload.bin';
    if (!U.isValidName(name)) throw new F.FileError('EINVAL', '文件名不合法：' + name);
    var bytes = await readBrowserFile(file);
    var target = U.joinPath(dir, name);
    var written = await this.fc.writeFile(target, bytes, {
      onProgress: function (p) {
        self._progress({ phase: 'upload', path: target, written: p.written, total: p.total });
      }
    });
    this.stats.uploads++;
    this.stats.bytesUp += written;
    return { path: target, name: name, size: written };
  };

  /** 写一段数据到文件（供"一键写入 PC2P4.TXT"用） */
  Files.prototype.writeBytes = async function (path, bytes, label) {
    var self = this;
    var written = await this.fc.writeFile(path, bytes, {
      onProgress: function (p) {
        self._progress({ phase: label || 'write', path: path, written: p.written, total: p.total });
      }
    });
    this.stats.bytesUp += written;
    return written;
  };

  /**
   * 一键写入 /PC2P4.TXT（内容带时间戳与随机标记），然后**回读校验**。
   * 这是"PC → P4 共享"最直接的证据：网页写、PC 端 MTP 卷里能打开同一文件。
   *
   * @param {string} [dir='/'] 写入目录（默认根目录，便于在 MTP 根卷里一眼看到）
   */
  Files.prototype.writeTestFile = async function (dir) {
    var target = U.joinPath(dir || '/', 'PC2P4.TXT');
    var stamp = U.fmtStamp();
    var marker = 'MP4-' + Date.now().toString(36).toUpperCase() + '-' +
      Math.random().toString(36).slice(2, 8).toUpperCase();
    var text = [
      'MeidaP4 · PC → P4 共享写入测试',
      '================================',
      '写入时间 (local): ' + stamp,
      '随机标记 (marker): ' + marker,
      '来源 (source): web/app.html 单文件版 · WebUSB IF2 · FILE 通道（xid 配对 + 可靠窗口）',
      '写入方式: OPEN(mode=1 截断) → 分块 WRITE(≤1012 B/帧) → CLOSE，随后回读校验',
      '',
      '验证方法：',
      '  1) 本页「刷新」后应看到 PC2P4.TXT，大小与写入字节数一致；',
      '  2) PC 端打开资源管理器里的 MeidaP4 MTP 卷（或 SD 卡读卡器），根目录应能打开同一文件；',
      '  3) 两边内容一致 ⇒ PC 与 P4 的文件级共享成立（不是独占）。',
      ''
    ].join('\r\n');
    var bytes = WU.utf8ToBytes(text);

    this._progress({ phase: 'test-write', path: target, written: 0, total: bytes.length });
    var written = await this.writeBytes(target, bytes, 'test-write');

    // 回读校验（真正证明"设备侧真的落盘了"，而不是只看 WRITE_RSP）
    var back = await this.readFile(target);
    var verified = back.bytes.length === bytes.length;
    if (verified) {
      for (var i = 0; i < bytes.length; i++) {
        if (back.bytes[i] !== bytes[i]) { verified = false; break; }
      }
    }
    this._progress({ phase: 'test-done', path: target, written: written, total: bytes.length });
    return {
      path: target,
      bytes: bytes.length,
      written: written,
      readBack: back.bytes.length,
      verified: verified,
      marker: marker,
      stamp: stamp
    };
  };

  /**
   * 把 FILE 通道错误翻译成中文可读提示。
   *
   * 设备侧的两种最常见状态必须区分（WS-F 约定）：
   *   · EBUSY (5)  = TF 卡未挂载（或文件服务忙）；
   *   · ENOENT (2) = 文件/目录不存在。
   * 两者给用户看的动作完全不同，混成一句"操作失败"会让现场排障走弯路。
   */
  Files.describeError = function (e) {
    if (!e) return '未知错误';
    if (e.name === 'ESTATUS') {
      switch (e.status) {
        case 5: return '设备回 EBUSY：TF 卡未挂载（或文件服务正忙）。请插好 TF 卡，等设备挂载后再点「刷新」。';
        case 2: return '设备回 ENOENT：文件/目录不存在（可能已被删除，或路径大小写不一致）。';
        case 4: return '设备回 ENOSPC：存储空间不足，写入被拒绝。';
        case 1: return '设备回 EINVAL：参数不合法（路径/句柄/长度不合法）。';
        case 3: return '设备回 EIO：设备侧 I/O 错误（读写失败）。';
        case 6: return '设备回 ENOTSUP：设备不支持该操作。';
        default: return '设备回 ' + (F.STATUS_NAME[e.status] || ('0x' + Number(e.status).toString(16))) +
          '（status=' + e.status + '）';
      }
    }
    if (e.name === 'ETIMEDOUT') return '超时：设备没有回响应（确认设备端 FILE 服务在跑、链路仍为 up）。';
    if (e.name === 'EBUSY') return '请求发不出去：链路未 up 或可靠窗口已满，稍后重试。';
    if (e.name === 'EABORT') return '请求被取消（链路重连或会话重置）。';
    if (e.name === 'ESTATUS') return U.errText(e);
    return U.errText(e);
  };

  /** 列目录时过滤掉 . 与 ..（导航由面包屑/上级按钮负责） */
  Files.filterNavigational = function (entries) {
    var out = [];
    for (var i = 0; i < entries.length; i++) {
      var n = entries[i].name;
      if (n === '.' || n === '..' || n === '') continue;
      out.push(entries[i]);
    }
    return out;
  };

  /** 目录项排序：目录在前，然后按名字（大小写不敏感，稳定） */
  Files.sortEntries = function (entries) {
    var arr = entries.slice();
    arr.sort(function (a, b) {
      if (!!a.isDir !== !!b.isDir) return a.isDir ? -1 : 1;
      var an = String(a.name).toLowerCase();
      var bn = String(b.name).toLowerCase();
      if (an < bn) return -1;
      if (an > bn) return 1;
      return 0;
    });
    return arr;
  };

  MP.Files = Files;
})(typeof window !== 'undefined' ? window : globalThis);
