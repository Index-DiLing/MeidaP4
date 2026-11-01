// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 单文件网页应用 · 桥接层
//
// 本文件在工具链里**紧跟** web/proto.js 与 web/file.js 的内联镜像之后执行，
// 作用只有一件事：把内联镜像里的**顶层词法绑定**收敛到一个显式命名空间 `window.MP`。
//
// 为什么不直接引用那些名字：
//   * proto.js / file.js 是 ES module，内联时只删掉 import/export 语法；
//   * 经典脚本里 `const` / `class` 属于全局词法环境，下一个 <script> 能看见，
//     但它们是**隐式全局**，一旦重名（比如我自己也想叫 CHANNEL）就会
//     "Identifier has already been declared" 直接白屏；
//   * 显式桥接后，应用层只认 `MP.WU.*` / `MP.FILE.*`，重名风险归零，
//     而且 `web/app_src/selftest.mjs` 可以在 node 里用同一个桥接跑纯逻辑测试。
//
// ⚠️ 这里列出的每个名字都必须在内联镜像里真实存在；写错一个，
//    页面在解析后会立刻抛 ReferenceError（自检的"vm 执行"用例会抓到）。
//    改 proto.js / file.js 的导出面时，**必须同步改这里**。

(function (root) {
  'use strict';

  var MP = root.MP = root.MP || {};

  MP.WU = {
    // 常量
    MAGIC: MAGIC,
    MAGIC0: MAGIC0,
    MAGIC1: MAGIC1,
    VERSION: VERSION,
    HDR_LEN: HDR_LEN,
    MAX_PAYLOAD: MAX_PAYLOAD,
    MAX_FRAME: MAX_FRAME,
    CHANNEL: CHANNEL,
    CHANNEL_NAMES: CHANNEL_NAMES,
    CHANNEL_MAX: CHANNEL_MAX,
    CHANNEL_INFO: CHANNEL_INFO,
    FLAGS: FLAGS,
    OP: OP,
    OP_NAMES: OP_NAMES,
    STATUS: STATUS,
    STATUS_NAMES: STATUS_NAMES,
    CAP: CAP,
    FILE_OP: FILE_OP,
    FILE_OP_NAMES: FILE_OP_NAMES,
    PROTO_VERSION: PROTO_VERSION,
    CTRL_HDR_LEN: CTRL_HDR_LEN,
    FILE_HDR_LEN: FILE_HDR_LEN,
    VIDEO_HDR_LEN: VIDEO_HDR_LEN,
    TELEM_LEN: TELEM_LEN,
    WINDOW: WINDOW,
    ACK_TIMEOUT_MS: ACK_TIMEOUT_MS,
    MAX_RETX: MAX_RETX,
    LINK_DOWN_MS: LINK_DOWN_MS,
    HELLO_PERIOD_MS: HELLO_PERIOD_MS,
    PING_PERIOD_MS: PING_PERIOD_MS,
    TELEM_PERIOD_MS: TELEM_PERIOD_MS,

    // 小工具与编解码
    toBytes: toBytes,
    hexToBytes: hexToBytes,
    bytesToHex: bytesToHex,
    utf8ToBytes: utf8ToBytes,
    bytesToUtf8: bytesToUtf8,
    rd16: rd16,
    rd32: rd32,
    wr16: wr16,
    wr32: wr32,
    crc16: crc16,
    crc16OfFrame: crc16OfFrame,
    encodeFrame: encodeFrame,
    fragmentMessage: fragmentMessage,

    // 解码 / 重组 / 可靠通道 / 会话
    FrameDecoder: FrameDecoder,
    Assembler: Assembler,
    ReliableChannel: ReliableChannel,
    LinkSession: LinkSession,

    // 载荷编解码
    ctrlPayload: ctrlPayload,
    parseCtrl: parseCtrl,
    filePayload: filePayload,
    parseFile: parseFile,
    parseTelem: parseTelem
  };

  MP.FILE = {
    FILE_NAME_LEN: FILE_NAME_LEN,
    FILE_ENTRY_LEN: FILE_ENTRY_LEN,
    MAX_WRITE_DATA: MAX_WRITE_DATA,
    MAX_READ_DATA: MAX_READ_DATA,
    OPEN_MODE: OPEN_MODE,
    DEFAULT_XID_TIMEOUT_MS: DEFAULT_XID_TIMEOUT_MS,
    DEFAULT_SEND_RETRY_MS: DEFAULT_SEND_RETRY_MS,
    STATUS_NAME: STATUS_NAME,
    FILE_CHANNEL: FILE_CHANNEL,

    FileError: FileError,
    FileClient: FileClient,

    encodeFileMessage: encodeFileMessage,
    decodeFileMessage: decodeFileMessage,
    encodeListEntries: encodeListEntries,
    decodeListEntries: decodeListEntries,
    encodeOpenRsp: encodeOpenRsp,
    decodeOpenRsp: decodeOpenRsp,
    encodeReadReq: encodeReadReq,
    decodeReadReq: decodeReadReq,
    encodeWriteReq: encodeWriteReq,
    decodeWriteReq: decodeWriteReq,
    encodeWriteRsp: encodeWriteRsp,
    decodeWriteRsp: decodeWriteRsp,
    encodeHandle: encodeHandle,
    encodeRenameBody: encodeRenameBody
  };

  // 供自检与页面诊断使用：一眼看出镜像版本（由 proto.js 的常量派生，不写死数字）
  MP.meta = {
    protoVersion: PROTO_VERSION,
    maxPayload: MAX_PAYLOAD,
    window: WINDOW,
    ackTimeoutMs: ACK_TIMEOUT_MS,
    linkDownMs: LINK_DOWN_MS,
    fileEntryLen: FILE_ENTRY_LEN
  };
})(typeof window !== 'undefined' ? window : globalThis);
