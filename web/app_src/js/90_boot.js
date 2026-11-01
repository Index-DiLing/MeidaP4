// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · 启动
//
// 只做一件事：DOM 就绪后调用 MP.UI.init()，并保证"初始化失败"不会白屏 ——
// 把异常原文显示在页面底部的 #boot-error 里（现场排障时用户能直接截图给 Lead）。

(function (root) {
  'use strict';

  var MP = root.MP;

  function showFatal(e) {
    var text = (e && (e.stack || e.message)) ? (e.stack || e.message) : String(e);
    try {
      var doc = root.document;
      var box = doc && doc.getElementById('boot-error');
      if (box) {
        box.textContent = '页面初始化失败（page init failed）\n\n' + text;
        box.style.display = 'block';
      }
    } catch (ignore) { /* 连兜底都失败就只剩控制台 */ }
    try { if (root.console && root.console.error) root.console.error('[MeidaP4] boot failed', e); }
    catch (ignore) { /* 忽略 */ }
  }

  function boot() {
    try {
      if (!MP || !MP.UI || typeof MP.UI.init !== 'function') {
        throw new Error('应用模块未就绪（MP.UI.init 缺失）：可能是内联脚本被截断，请用 tools/build_web.py 重新生成 app.html');
      }
      MP.UI.init();
    } catch (e) {
      showFatal(e);
    }
  }

  var doc = root.document;
  if (doc && doc.readyState === 'loading' && doc.addEventListener) {
    doc.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }

  MP.boot = { boot: boot, showFatal: showFatal };
})(typeof window !== 'undefined' ? window : globalThis);
