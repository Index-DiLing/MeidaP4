// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · 通用小工具（不碰 DOM、不碰协议状态）
//
// 纯函数为主：格式化、路径处理、错误文本。方便在 node 自检里直接调用。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};
  var U = MP.util = {};

  /* ------------------------------------------------------------------ 格式 */

  U.pad2 = function (n) { return (n < 10 ? '0' : '') + n; };
  U.pad3 = function (n) { return (n < 10 ? '00' : n < 100 ? '0' : '') + n; };

  /** 本地时间 HH:MM:SS.mmm（日志时间戳） */
  U.fmtClock = function (t) {
    var d = new Date(t === undefined ? Date.now() : t);
    return U.pad2(d.getHours()) + ':' + U.pad2(d.getMinutes()) + ':' + U.pad2(d.getSeconds()) +
      '.' + U.pad3(d.getMilliseconds());
  };

  /** 本地时间 YYYY-MM-DD HH:MM:SS（写文件用，带时区偏移，便于肉眼核对） */
  U.fmtStamp = function (t) {
    var d = new Date(t === undefined ? Date.now() : t);
    var offMin = -d.getTimezoneOffset();
    var sign = offMin >= 0 ? '+' : '-';
    var ah = Math.floor(Math.abs(offMin) / 60);
    var am = Math.abs(offMin) % 60;
    return d.getFullYear() + '-' + U.pad2(d.getMonth() + 1) + '-' + U.pad2(d.getDate()) + ' ' +
      U.pad2(d.getHours()) + ':' + U.pad2(d.getMinutes()) + ':' + U.pad2(d.getSeconds()) +
      ' (UTC' + sign + U.pad2(ah) + ':' + U.pad2(am) + ')';
  };

  /** 1234567 -> "1,234,567"（不用 toLocaleString，避免不同环境分隔符不一致） */
  U.fmtInt = function (n) {
    var v = Number(n);
    if (!isFinite(v)) return String(n);
    var neg = v < 0;
    var s = String(Math.abs(Math.trunc(v)));
    var out = '';
    for (var i = 0; i < s.length; i++) {
      if (i > 0 && (s.length - i) % 3 === 0) out += ',';
      out += s[i];
    }
    return (neg ? '-' : '') + out;
  };

  /** 字节数 → 便于阅读的 B / KB / MB */
  U.fmtBytes = function (n, digits) {
    var v = Number(n);
    if (!isFinite(v)) return String(n);
    var d = (digits === undefined) ? 1 : digits;
    if (Math.abs(v) < 1024) return Math.trunc(v) + ' B';
    if (Math.abs(v) < 1024 * 1024) return (v / 1024).toFixed(d) + ' KB';
    if (Math.abs(v) < 1024 * 1024 * 1024) return (v / 1024 / 1024).toFixed(d) + ' MB';
    return (v / 1024 / 1024 / 1024).toFixed(d) + ' GB';
  };

  /** 毫秒 → "1h 02m 03s" / "02m 03s" / "3.2s" */
  U.fmtDuration = function (ms) {
    var v = Number(ms) || 0;
    if (v < 1000) return v + ' ms';
    var s = Math.floor(v / 1000);
    var h = Math.floor(s / 3600);
    var m = Math.floor((s % 3600) / 60);
    var sec = s % 60;
    if (h > 0) return h + 'h ' + U.pad2(m) + 'm ' + U.pad2(sec) + 's';
    if (m > 0) return m + 'm ' + U.pad2(sec) + 's';
    return sec + 's';
  };

  /** 码率：bytes/秒 → "123.4 kbps"（1 kbit = 1000 bit） */
  U.fmtKbps = function (bytesPerSec) {
    var v = Number(bytesPerSec) || 0;
    var kbps = v * 8 / 1000;
    if (kbps >= 1000) return (kbps / 1000).toFixed(2) + ' Mbps';
    if (kbps >= 10) return kbps.toFixed(0) + ' kbps';
    return kbps.toFixed(1) + ' kbps';
  };

  /** 错误对象 → 可读文本（永远不返回 "undefined"） */
  U.errText = function (e) {
    if (!e) return '(未知错误)';
    var name = e.name || (e.constructor && e.constructor.name) || 'Error';
    var msg = e.message || String(e);
    if (!msg) return name;
    return (name === 'Error' ? '' : name + ': ') + msg;
  };

  /** 数组/类数组 → 普通数组（Uint8Array 也适用） */
  U.toArray = function (x) {
    var out = [];
    if (!x) return out;
    for (var i = 0; i < x.length; i++) out.push(x[i]);
    return out;
  };

  /** 拼接若干 Uint8Array */
  U.concatBytes = function (list) {
    var n = 0, i;
    for (i = 0; i < list.length; i++) n += list[i].length;
    var out = new Uint8Array(n);
    var off = 0;
    for (i = 0; i < list.length; i++) { out.set(list[i], off); off += list[i].length; }
    return out;
  };

  /** 字节数组是否以给定尾部结尾（MJPEG 用 0xFF 0xD9 判定一帧完整） */
  U.endsWith = function (bytes, tail) {
    if (!bytes || !tail || bytes.length < tail.length) return false;
    var off = bytes.length - tail.length;
    for (var i = 0; i < tail.length; i++) if (bytes[off + i] !== tail[i]) return false;
    return true;
  };

  /* ------------------------------------------------------------------ 路径 */
  // 设备侧路径一律用 POSIX 风格、绝对路径、以 '/' 开头；根目录就是 "/"。
  // 这里不做 URL 编码，也不允许反斜杠（Windows 用户输入会转成 '/'）。

  U.normalizePath = function (p) {
    var s = String(p === undefined || p === null ? '/' : p).replace(/\\/g, '/');
    var parts = s.split('/');
    var out = [];
    for (var i = 0; i < parts.length; i++) {
      var seg = parts[i];
      if (!seg || seg === '.') continue;
      if (seg === '..') { out.pop(); continue; }
      out.push(seg);
    }
    return '/' + out.join('/');
  };

  U.joinPath = function (dir, name) {
    var d = U.normalizePath(dir);
    var n = String(name === undefined || name === null ? '' : name).replace(/\\/g, '/');
    if (n.charAt(0) === '/') return U.normalizePath(n);
    return U.normalizePath((d === '/' ? '' : d) + '/' + n);
  };

  U.parentPath = function (p) {
    var s = U.normalizePath(p);
    if (s === '/') return '/';
    var i = s.lastIndexOf('/');
    return i <= 0 ? '/' : s.slice(0, i);
  };

  U.baseName = function (p) {
    var s = U.normalizePath(p);
    if (s === '/') return '/';
    return s.slice(s.lastIndexOf('/') + 1);
  };

  U.splitPath = function (p) {
    var s = U.normalizePath(p);
    if (s === '/') return [];
    return s.slice(1).split('/');
  };

  /** 目录项名字是否合法（新建/重命名用）：非空、无路径分隔符、不是 . / .. */
  U.isValidName = function (name) {
    var s = String(name === undefined || name === null ? '' : name);
    if (!s || s === '.' || s === '..') return false;
    if (s.length > 200) return false;
    if (/[\/\\\u0000]/.test(s)) return false;
    return true;
  };

  /* ------------------------------------------------------------------ 杂项 */

  U.sleep = function (ms) {
    return new Promise(function (resolve) { root.setTimeout(resolve, ms); });
  };

  /** 只保留最近 n 条，避免长时间运行把内存吃光 */
  U.trimArray = function (arr, n) {
    if (arr.length > n) arr.splice(0, arr.length - n);
    return arr;
  };

  /** 求平均值（空数组返回 0） */
  U.mean = function (arr) {
    if (!arr || !arr.length) return 0;
    var s = 0;
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s / arr.length;
  };

})(typeof window !== 'undefined' ? window : globalThis);
