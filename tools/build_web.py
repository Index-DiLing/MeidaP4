#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeidaP4 · 单文件网页应用生成器（web/app.html）

为什么要有它
------------
`web/app.html` 必须能**从 SD 卡双击、以 file:// 打开**：
  * `file://` 下 Chrome **禁止**加载外部 ES module / 外部脚本 / 外部样式；
  * 也不能 fetch 本地文件。
所以交付物必须是"一个 HTML 文件、里面内联了全部 CSS 与 JS"。
但把上千行 JS 全塞进一个文件里维护是不可接受的，因此：

  web/app_src/app.html.tpl     页面骨架（含占位符）
  web/app_src/app.css          样式
  web/app_src/js/NN_*.js       模块化源码（经典脚本 + window.MP 命名空间）
        ↓  tools/build_web.py
  web/app.html                 单文件交付物（生成物，不要手改）

两条内联规则
------------
1. `web/proto.js` / `web/file.js` 是本项目冻结的**权威实现**，本脚本不做任何改写，
   只做 `strip_module()`：删掉 `import`/`export` 语法、补 `'use strict'`，其余**逐字节保留**。
   ⇒ app.html 里的协议实现与 proto.js / file.js 逐字节等价，
     `web/app_src/selftest.mjs` 会重新计算一遍并断言"逐字节一致"。
2. app_src/js/*.js 原样内联（它们本来就是经典脚本）。

确定性（可重复生成）
--------------------
输出里**没有时间戳、没有绝对路径、没有环境相关信息**；换行统一为 LF。
同一份源码在 Windows / Linux / macOS 上生成的 app.html 逐字节相同。
`--check` 可以在 CI / 自检里验证 "app.html 与源码同步"（不同步 → exit 1）。

用法
----
    python tools/build_web.py            # 生成 web/app.html
    python tools/build_web.py --check    # 只校验是否同步（不写文件）
    python tools/build_web.py --quiet    # 少打印
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

# Windows 控制台/管道默认不是 UTF-8：尽量把本脚本的提示以 UTF-8 输出，
# 免得中文提示在 GBK 终端里变成乱码（失败也无所谓，输出内容不影响生成结果）。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # pragma: no cover - 老 Python / 特殊终端
        pass


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
APP_SRC = WEB / "app_src"
TEMPLATE = APP_SRC / "app.html.tpl"
STYLESHEET = APP_SRC / "app.css"
OUTPUT = WEB / "app.html"

PROTO_JS = WEB / "proto.js"
FILE_JS = WEB / "file.js"

# ⚠️ 这两条横幅必须与 web/app_src/selftest.mjs 里的同名常量**完全一致**，
#    否则"逐字节一致"断言会失败（这正是我们想要的：它防两处漂移）。
PROTO_BANNER = "[内联镜像 of web/proto.js] 由 tools/build_web.py 生成，请勿手改"
FILE_BANNER = "[内联镜像 of web/file.js] 由 tools/build_web.py 生成，请勿手改"

# 内联顺序 = 依赖顺序（经典脚本，共享同一个全局词法环境）。
#   (相对 app_src 的路径, <script id>)
MODULES = [
    ("js/05_bridge.js", "app-bridge"),
    ("js/10_util.js", "app-util"),
    ("js/20_usb.js", "app-usb"),
    ("js/30_link.js", "app-link"),
    ("js/40_video.js", "app-video"),
    ("js/50_files.js", "app-files"),
    ("js/60_ui.js", "app-ui"),
    ("js/90_boot.js", "app-boot"),
]


# ---------------------------------------------------------------------------
# 读取 + 规范化
# ---------------------------------------------------------------------------
def read_text(path: Path) -> str:
    """读 UTF-8 文本并统一换行为 LF（保证跨平台确定性）。"""
    raw = path.read_text(encoding="utf-8")
    return raw.replace("\r\n", "\n").replace("\r", "\n")


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def block_body(text: str) -> str:
    """把一段源码变成 script/style 元素内的内容：去首尾空行 + 单个换行结尾。"""
    return "\n" + text.strip("\n") + "\n"


# ---------------------------------------------------------------------------
# ESM → 经典脚本（与 web/selfcheck.mjs 的 stripModule() 同一算法）
# ---------------------------------------------------------------------------
_IMPORT_FROM = re.compile(r"from\s+['\"][^'\"]+['\"]\s*;?\s*$")
_IMPORT_SIDE = re.compile(r"^import\s+['\"]")
_EXPORT_DEFAULT = re.compile(r"^export default\s+(\w+)\s*;?\s*$")
_EXPORT = re.compile(r"^export\s+")


def strip_module(text: str, banner: str) -> str:
    """把 ES module 源码转成"经典脚本"形态：只去掉 import/export 语法。

    逐行处理，其余每一个字节都不改动 —— 这就是"内联副本与源文件等价"的可验证定义。
    """
    lines = text.replace("\r\n", "\n").split("\n")
    out = ["'use strict';", "// " + banner]
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if re.match(r"^import\s", line):
            if _IMPORT_FROM.search(line) or _IMPORT_SIDE.match(line):
                i += 1
                continue
            # 多行 import：吃到 `from '...'` 那一行为止
            i += 1
            while i < n and not _IMPORT_FROM.search(lines[i]):
                i += 1
            if i < n:
                i += 1
            continue
        m = _EXPORT_DEFAULT.match(line)
        if m:
            out.append("// [内联镜像] export default " + m.group(1) + ";  ← 经典脚本里无默认导出，忽略")
            i += 1
            continue
        if _EXPORT.match(line):
            out.append(_EXPORT.sub("", line, count=1))
            i += 1
            continue
        out.append(line)
        i += 1
    return "\n".join(out)


# ---------------------------------------------------------------------------
# 生成
# ---------------------------------------------------------------------------
def build_html() -> str:
    if not TEMPLATE.is_file():
        raise SystemExit("缺少模板：%s" % TEMPLATE)
    for rel, _sid in MODULES:
        if not (APP_SRC / rel).is_file():
            raise SystemExit("缺少源码：%s" % (APP_SRC / rel))
    for p in (PROTO_JS, FILE_JS):
        if not p.is_file():
            raise SystemExit("缺少权威实现：%s" % p)

    template = read_text(TEMPLATE)
    css = read_text(STYLESHEET)

    # --- 1) 两个"逐字节等价镜像" -------------------------------------------------
    proto_src = read_text(PROTO_JS)
    file_src = read_text(FILE_JS)
    proto_inline = strip_module(proto_src, PROTO_BANNER)
    file_inline = strip_module(file_src, FILE_BANNER)

    sources: list[tuple[str, str, str]] = [
        # (显示用路径, script id, 内容)
        ("web/proto.js", "wu-proto-inline", proto_inline),
        ("web/file.js", "wu-file-inline", file_inline),
    ]
    # --- 2) 应用模块（原样内联） -------------------------------------------------
    for rel, sid in MODULES:
        sources.append(("web/app_src/" + rel, sid, read_text(APP_SRC / rel)))

    # --- 3) 组装 <script> 块 ------------------------------------------------------
    chunks: list[str] = []
    manifest: list[str] = []
    for disp, sid, content in sources:
        body = block_body(content)
        low = body.lower()
        if "</script" in low:
            raise SystemExit("%s 里含 </script，无法内联（请拆开该字符串）" % disp)
        if "<!--" in body:
            raise SystemExit("%s 里含 <!--，会破坏 HTML 解析（请拆开该注释串）" % disp)
        digest = sha256(body.strip("\n"))
        manifest.append("  %-34s id=%-16s sha256=%s  %6d B" % (disp, sid, digest[:16], len(body)))
        chunks.append(
            "<!-- @source %s | sha256=%s | generated by tools/build_web.py -->\n"
            '<script id="%s">%s</script>' % (disp, digest, sid, body)
        )
    scripts = "\n".join(chunks)
    if "<!--@SCRIPTS@-->" not in template:
        raise SystemExit("app.html.tpl 缺少占位符 <!--@SCRIPTS@-->")
    if "/*@STYLE@*/" not in template:
        raise SystemExit("app.html.tpl 缺少占位符 /*@STYLE@*/")

    header = (
        "<!--\n"
        "  MeidaP4 · WebUSB 单文件网页应用（视频画面 / TF 卡文件浏览 / LOG / 遥测）\n"
        "\n"
        "  ⚠️ 本文件由 tools/build_web.py 自动生成，请勿手改。\n"
        "     改源码 → web/app_src/**，然后重新运行：python tools/build_web.py\n"
        "     校验 app.html 是否与源码同步：python tools/build_web.py --check\n"
        "\n"
        "  为什么是单文件：页面要从 SD 卡以 file:// 双击打开，而 file:// 下\n"
        "    Chrome 禁止加载外部 ES module / 外部脚本 / 外部样式，也不允许 fetch 本地文件。\n"
        "  web/proto.js 与 web/file.js 是冻结的权威实现，此处内联的是它们的\n"
        "  strip_module() 镜像（只删 import/export 语法，其余逐字节保留）。\n"
        "  web/app_src/selftest.mjs 会重新计算并断言逐字节一致。\n"
        "\n"
        "  内联清单（按依赖顺序）：\n"
        + "\n".join(manifest)
        + "\n-->\n"
    )

    html = template.replace("/*@STYLE@*/", block_body(css)).replace("<!--@SCRIPTS@-->", scripts)

    # --- 4) 自证：单文件、无外部依赖、无 ESM -------------------------------
    problems: list[str] = []
    low = html.lower()
    if re.search(r"<script[^>]*\bsrc\s*=", low):
        problems.append("出现 <script src=...>（外部脚本）")
    if re.search(r"<link\b", low):
        problems.append("出现 <link>（外部资源）")
    if re.search(r"<img\b", low):
        problems.append("出现 <img>（外部图片）")
    if re.search(r"<iframe\b", low):
        problems.append("出现 <iframe>")
    if re.search(r"type\s*=\s*['\"]module['\"]", low):
        problems.append("出现 type=module")
    if re.search(r"^import\s", html, re.MULTILINE):
        problems.append("出现顶层 import 语句")
    if "@import" in low:
        problems.append("CSS 出现 @import")
    if re.search(r"url\(\s*['\"]?http", low):
        problems.append("CSS 出现 url(http…)")
    if re.search(r"\bfetch\s*\(", html):
        problems.append("出现 fetch()（file:// 下不允许取本地/网络资源）")
    if "xmlhttprequest" in low:
        problems.append("出现 XMLHttpRequest")
    if re.search(r"<script[^>]*\bsrc\s*=|href\s*=\s*['\"]https?:", low):
        problems.append("出现外部 http(s) 资源引用")
    if problems:
        raise SystemExit("单文件约束被破坏：\n  " + "\n  ".join(problems))

    return header + html


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="生成 web/app.html（单文件、无外部依赖）")
    ap.add_argument("--check", action="store_true", help="只校验 app.html 是否与源码同步（不同步 exit 1）")
    ap.add_argument("--quiet", action="store_true", help="少打印")
    args = ap.parse_args(argv)

    html = build_html()
    data = html.encode("utf-8")
    old = OUTPUT.read_bytes() if OUTPUT.is_file() else None

    if args.check:
        if old is None:
            print("[build_web] FAIL: web/app.html 不存在（运行 python tools/build_web.py）")
            return 1
        if old == data:
            print("[build_web] OK: web/app.html 与 web/app_src/** 同步（%d B）" % len(data))
            return 0
        print("[build_web] FAIL: web/app.html 与源码不同步（现有 %s B / 期望 %s B）"
              % (len(old), len(data)))
        print("           运行：python tools/build_web.py")
        return 1

    OUTPUT.write_bytes(data)
    if not args.quiet:
        changed = "更新" if (old is not None and old != data) else ("未变化" if old == data else "新建")
        print("[build_web] %s: %s（%d B）" % (changed, OUTPUT, len(data)))
        for rel, _sid in MODULES:
            print("            - %s" % rel)
        print("            - 内联镜像：web/proto.js、web/file.js（逐字节等价，仅去 import/export）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
