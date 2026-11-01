// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 集成测试用桥接（verifier；只在 `-DUSE_REAL_NCM=1` 的 host 测试里链接）
//
// 目的：让 `test/host/test_composite_desc.c` 能注册 **WS-net 真实的** `usb_net_ncm_register()`
// （即真实 NCM 描述符片段）到真实的 `usb_composite_desc.c` 装配器上，从而验证：
//   * 真片段能否通过**真** `validate_fragment()`（含 §1.1 的 alt=1 复用接口号路径）；
//   * I1 插出的 IAD 在真实组装里是 first=3 / count=2 / 02-0D-00；
//   * 字符串恰好 8 条（内容去重是否真的成立）；
//   * FIFO 预算与字符串表在"真片段"下仍然不超限。
//
// `usb_net_ncm.c` 只需要 `g_usbnet` 的 `seg_size` 与 `mac_str` 两个字段（见其 :165/:250）。
// 这里按 `usb_net_priv.h` 的定义给一份最小单例 —— **不是**产品代码，也不参与固件构建。

#include "usb_net_priv.h"
#include "usb_composite.h"
#include "usb_composite_internal.h"

usb_net_ctx_t g_usbnet = {
    .mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
    .mac_str = "AABBCCDDEEFF",
    .seg_size = 1514,          /* = tinyusb CFG_TUD_NET_MTU / usb_net_priv.h：USB_NET_NCM_SEG_SIZE */
};

/* ---------------------------------------------------------------------------
 * 公开 API 的薄桥：`usb_net_ncm.c` 走的是**公开**注册入口 `usb_composite_register()`，
 * 而它的实现在 IDF 侧的 `usb_composite.c`（无法原生编译）。
 * 这里把公开入口转发到本组件内部的纯计算实现 `uc_register()`。
 *
 * ⚠️ 等价性条件（本文件成立的前提，必须保持检查）：
 *   1. `usb_composite.c` 的 `usb_composite_register()` 目前只是
 *      `return uc_register(fn);`（+ 失败日志）。若它将来在转发**之前**做了任何语义动作
 *      （校验、转换、拷贝），本集成测试就**不再等价于固件路径**，需要同步。
 *   2. **F9 的拷贝必须留在 `uc_register()` 内部**（现状：`usb_composite_desc.c:269-271`
 *      `static usb_function_t s_fn_copy[...]; s_fn_copy[n] = *fn;`）——
 *      因为本桥是从公开入口**直接转发到** `uc_register()`，会"越过" `usb_composite.c` 那一层。
 *      若将来把拷贝搬到 `usb_composite.c`（公开层），本桥就会绕过它、集成测试的等价性**悄悄失效**
 *      （按 F9 的教训：这种"过期等价"正是最难发现的一类）。
 * ------------------------------------------------------------------------- */
esp_err_t usb_composite_register(const usb_function_t *fn)
{
    return uc_register(fn);
}
