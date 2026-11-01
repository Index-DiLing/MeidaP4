/*
 * MeidaP4 · WS-net · **离线** NCM 描述符自检（host gcc，不需要 IDF / 不需要设备）
 *
 * 目的：把 `components/usb_net/usb_net_ncm.c` 产出的 77 字节片段与**独立手写**的期望值逐字节比对，
 *       并把 I1 会插的 IAD 一起还原成 85 字节的最终形态。
 *
 * 为什么值得单独做：
 *   - 描述符写错在**离线编译期是完全看不见的**，而设备未连接时真机也验不了；
 *   - 这份期望值是从 CDC-NCM 1.0 / USB 2.0 §9.6 + tinyusb `usbd.h:1080-1102` 手工敲出来的
 *     （不是从被测代码复制的），所以"两边一起写错"的概率很低；
 *   - verifier 可以直接跑它，不需要占用构建槽位。
 *
 * 编译运行：
 *   pwsh -File components/usb_net/test/host/run.ps1        # 推荐
 * 或手工：
 *   gcc -std=gnu99 -Wall -Wextra \
 *       -I components/usb_net/test/host/stubs \
 *       -I components/usb_net/include \
 *       -I components/usb_composite/include \
 *       components/usb_net/test/host/test_ncm_desc.c \
 *       components/usb_net/usb_net_ncm.c -o out.exe && ./out.exe
 *
 * ⚠️ 覆盖边界：**只**覆盖描述符字节与 I1 注册形态，**不**覆盖 esp_netif/NAPT/DHCP/DNS 行为，
 *    更不能替代真机枚举（离线 PASS ≠ 真机通过）。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb_composite.h"
#include "usb_net_priv.h"

/* ============ 替身：I1 的注册入口（捕获 usb_function_t） ============ */
static usb_function_t g_fn;
static int g_register_calls;

esp_err_t usb_composite_register(const usb_function_t *fn)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    g_fn = *fn;
    g_register_calls++;
    return ESP_OK;
}

/* ============ 生产代码里的单例（正常情况下由 usb_net.c 定义） ============ */
usb_net_ctx_t g_usbnet;

/* ============ 断言 ============ */
static int g_fail;
static int g_pass;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            printf("  [FAIL] " __VA_ARGS__);                                \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/* ============ 期望值（独立手写，来源见注释） ============ */
/* HS（bulk 512）：77 字节 = 9+5+5+13+6+7+9+9+7+7（tinyusb usbd.h:1064 TUD_CDC_NCM_DESC_LEN - 8(IAD)） */
static const uint8_t EXP_HS[77] = {
    /* [1] CDC 控制接口：len,type,itf=3,alt=0,nEP=1,class=02(CDC),sub=0D(NCM),proto=00,str=6 */
    9, 0x04, 3, 0, 1, 0x02, 0x0D, 0x00, 6,
    /* [2] Header Functional Descriptor：bcdCDC = 0x0110 (LE: 10 01) */
    5, 0x24, 0x00, 0x10, 0x01,
    /* [3] Union Functional Descriptor：master=3(控制), slave=4(数据) */
    5, 0x24, 0x06, 3, 4,
    /* [4] Ethernet Networking (13B)：iMACAddress=7, bmEthernetStatistics=0,0,0,0,
     *     wMaxSegmentSize=1514(0x05EA → EA 05), wNumberMCFilters=0,0, bNumberPowerFilters=0 */
    13, 0x24, 0x0F, 7, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0,
    /* [5] NCM Functional (6B)：bcdNCM=0x0100(00 01), caps=ETH_FILTER|NTB_INPUT_SIZE=0x21 */
    6, 0x24, 0x1A, 0x00, 0x01, 0x21,
    /* [6] 通知端点：0x85(int IN EP5), attr=03(interrupt), mps=16(10 00), interval=8ms */
    7, 0x05, 0x85, 0x03, 16, 0, 8,
    /* [7] 数据接口 alt0：itf=4, alt=0, nEP=0, class=0A(CDC-Data), sub=00, proto=01(NTB), str=0 */
    9, 0x04, 4, 0, 0, 0x0A, 0x00, 0x01, 0,
    /* [8] 数据接口 alt1：itf=4, alt=1, nEP=2（同一接口号的另一套设置） */
    9, 0x04, 4, 1, 2, 0x0A, 0x00, 0x01, 0,
    /* [9] EP IN 0x86(bulk EP6), attr=02(bulk), mps=512(0x0200 → 00 02), interval=0 */
    7, 0x05, 0x86, 0x02, 0x00, 0x02, 0,
    /* [10] EP OUT 0x06(bulk EP6), attr=02(bulk), mps=512, interval=0 */
    7, 0x05, 0x06, 0x02, 0x00, 0x02, 0,
};

/* FS（bulk 64）：只有两个 bulk 端点的 wMaxPacketSize 不同 */
static const uint8_t EXP_FS[77] = {
    9, 0x04, 3, 0, 1, 0x02, 0x0D, 0x00, 6,
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x06, 3, 4,
    13, 0x24, 0x0F, 7, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0,
    6, 0x24, 0x1A, 0x00, 0x01, 0x21,
    7, 0x05, 0x85, 0x03, 16, 0, 8,
    9, 0x04, 4, 0, 0, 0x0A, 0x00, 0x01, 0,
    9, 0x04, 4, 1, 2, 0x0A, 0x00, 0x01, 0,
    7, 0x05, 0x86, 0x02, 64, 0, 0,          /* FS bulk = 64（USB 2.0 FS 上限） */
    7, 0x05, 0x06, 0x02, 64, 0, 0,
};

static int diff_report(const char *tag, const uint8_t *got, const uint8_t *exp, size_t n)
{
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (got[i] != exp[i]) {
            if (bad < 12) {
                printf("  [DIFF] %s off=%2zu got=0x%02X exp=0x%02X\n", tag, i, got[i], exp[i]);
            }
            bad++;
        }
    }
    return bad;
}

static void hexdump(const char *tag, const uint8_t *b, size_t n)
{
    printf("  %s (%zu B):", tag, n);
    for (size_t i = 0; i < n; i++) {
        printf("%s%02X", (i % 16 == 0) ? "\n    " : " ", b[i]);
    }
    printf("\n");
}

int main(void)
{
    printf("=== MeidaP4 WS-net · NCM 描述符离线自检（host gcc，无需 IDF） ===\n");

    memset(&g_usbnet, 0, sizeof(g_usbnet));
    g_usbnet.seg_size = USB_NET_NCM_SEG_SIZE;
    memcpy(g_usbnet.mac_str, "AABBCCDDEEFF", sizeof("AABBCCDDEEFF"));   /* 含 NUL */

    /* ---------- 1) I1 注册形态 ---------- */
    printf("\n[1] 通过 I1 注册的 usb_function_t\n");
    esp_err_t err = usb_net_ncm_register();
    CHECK(err == ESP_OK, "usb_net_ncm_register() = %d", err);
    CHECK(g_register_calls == 1, "usb_composite_register 调用次数 = %d（期望 1）", g_register_calls);

    CHECK(g_fn.itf_count == 2, "itf_count = %u（NCM = 控制+数据 2 个接口）", (unsigned)g_fn.itf_count);
    CHECK(g_fn.use_iad == true, "use_iad 必须 true（冻结表 §6：NCM 必须带 IAD）");
    CHECK(g_fn.itf_string && strcmp(g_fn.itf_string, "MeidaP4 Network") == 0,
          "itf_string = %s（期望 MeidaP4 Network）", g_fn.itf_string ? g_fn.itf_string : "(null)");
    CHECK(g_fn.extra_string && strcmp(g_fn.extra_string, "AABBCCDDEEFF") == 0,
          "extra_string = %s（iMACAddress 必须是 12 个十六进制字符）",
          g_fn.extra_string ? g_fn.extra_string : "(null)");
    CHECK(g_fn.is_vendor == false, "is_vendor 必须 false（NCM 不走 vendor 驱动）");
    CHECK(g_fn.wcid == false, "wcid 必须 false（NCM 用 Windows 收件箱 usbncm.sys）");
    CHECK(g_fn.on_out == NULL, "on_out 必须 NULL（OUT 端点归 esp_tinyusb 的 net 类驱动，不走 I1 数据面）");
    CHECK(g_fn.init && g_fn.build_desc, "init/build_desc 必须非空");

    CHECK(g_fn.ep_in_count == 2 && g_fn.ep_out_count == 1,
          "端点数量 IN=%u OUT=%u（期望 2/1）",
          (unsigned)g_fn.ep_in_count, (unsigned)g_fn.ep_out_count);
    CHECK(g_fn.ep_in_req[0].xfer == USB_EP_XFER_INTERRUPT &&
          g_fn.ep_in_req[0].mps_hs == USB_NET_NCM_NOTIF_MPS &&
          g_fn.ep_in_req[0].mps_fs == USB_NET_NCM_NOTIF_MPS &&
          g_fn.ep_in_req[0].interval == USB_NET_NCM_NOTIF_INTERVAL,
          "IN[0] 应为 interrupt 16/16 interval %u（冻结表 §2：EP5）",
          (unsigned)USB_NET_NCM_NOTIF_INTERVAL);
    CHECK(g_fn.ep_in_req[1].xfer == USB_EP_XFER_BULK &&
          g_fn.ep_in_req[1].mps_hs == USB_NET_NCM_BULK_MPS_HS &&
          g_fn.ep_in_req[1].mps_fs == USB_NET_NCM_BULK_MPS_FS,
          "IN[1] 应为 bulk 512/64（冻结表 §2：EP6 IN）");
    CHECK(g_fn.ep_out_req[0].xfer == USB_EP_XFER_BULK &&
          g_fn.ep_out_req[0].mps_hs == USB_NET_NCM_BULK_MPS_HS &&
          g_fn.ep_out_req[0].mps_fs == USB_NET_NCM_BULK_MPS_FS,
          "OUT[0] 应为 bulk 512/64（冻结表 §2：EP6 OUT）");

    CHECK(g_fn.ep_out_same_num_as_in[0] == 1,
          "ep_out_same_num_as_in[0] = %u（期望 1 → OUT 复用 IN[1] 的号 = EP6）",
          (unsigned)g_fn.ep_out_same_num_as_in[0]);
    for (int i = 1; i < USB_COMPOSITE_MAX_EP_PER_DIR; i++) {
        CHECK(g_fn.ep_out_same_num_as_in[i] == USB_EP_NO_PAIR,
              "ep_out_same_num_as_in[%d] = %u（整表必须初始化成 USB_EP_NO_PAIR，否则会被当成复用 IN[0]）",
              i, (unsigned)g_fn.ep_out_same_num_as_in[i]);
    }

    /* ---------- 2) 描述符片段（按冻结分配：IF3/IF4、EP5/EP6、str 6/7） ---------- */
    usb_fn_alloc_t a;
    memset(&a, 0, sizeof(a));
    a.itf_count = 2;
    a.itf[0] = 3;
    a.itf[1] = 4;
    a.ep_in_count = 2;
    a.ep_in[0] = (uint8_t)(0x80 | 5);
    a.ep_in[1] = (uint8_t)(0x80 | 6);
    a.ep_out_count = 1;
    a.ep_out[0] = 6;
    a.itf_str_idx = 6;
    a.extra_str_idx = 7;

    uint8_t buf[128];
    size_t len = 0;

    printf("\n[2] HS 描述符片段\n");
    memset(buf, 0xAA, sizeof(buf));
    err = g_fn.build_desc(true, &a, buf, sizeof(buf), &len);
    CHECK(err == ESP_OK, "HS build_desc err=%d", err);
    CHECK(len == sizeof(EXP_HS), "HS 片段长度 = %zu（期望 %zu）", len, sizeof(EXP_HS));
    int bad_hs = diff_report("HS", buf, EXP_HS, sizeof(EXP_HS));
    CHECK(bad_hs == 0, "HS 片段有 %d 字节与规范期望不一致", bad_hs);
    hexdump("HS fragment", buf, len);

    printf("\n[3] FS 描述符片段\n");
    memset(buf, 0xAA, sizeof(buf));
    err = g_fn.build_desc(false, &a, buf, sizeof(buf), &len);
    CHECK(err == ESP_OK, "FS build_desc err=%d", err);
    CHECK(len == sizeof(EXP_FS), "FS 片段长度 = %zu（期望 %zu）", len, sizeof(EXP_FS));
    int bad_fs = diff_report("FS", buf, EXP_FS, sizeof(EXP_FS));
    CHECK(bad_fs == 0, "FS 片段有 %d 字节与规范期望不一致", bad_fs);

    /* ---------- 4) IAD（I1 会插；它的类字段抄自片段第一个接口） ---------- */
    printf("\n[4] IAD（由 I1 插入，类字段抄自片段第一个接口描述符）\n");
    uint8_t final85[85];
    final85[0] = 8;
    final85[1] = 0x0B;
    final85[2] = a.itf[0];          /* bFirstInterface */
    final85[3] = a.itf_count;       /* bInterfaceCount */
    final85[4] = buf[5];            /* class ← 片段首接口 */
    final85[5] = buf[6];            /* subclass */
    final85[6] = buf[7];            /* protocol */
    final85[7] = 0;
    memcpy(final85 + 8, buf, sizeof(EXP_FS));
    CHECK(final85[4] == 0x02 && final85[5] == 0x0D && final85[6] == 0x00,
          "IAD 类字段 = %02X/%02X/%02X（期望 02/0D/00，冻结表 §6）",
          final85[4], final85[5], final85[6]);
    CHECK(final85[2] == 3 && final85[3] == 2, "IAD first=%u count=%u（期望 3/2）",
          final85[2], final85[3]);
    hexdump("HS config (IAD+77)", final85, sizeof(final85));

    /* ---------- 5) 反向用例：参数不足/缓冲不足必须被拒 ---------- */
    printf("\n[5] 反向用例\n");
    usb_fn_alloc_t bad = a;
    bad.itf_count = 1;
    err = g_fn.build_desc(true, &bad, buf, sizeof(buf), &len);
    CHECK(err == ESP_ERR_INVALID_STATE, "itf_count=1 应返回 ESP_ERR_INVALID_STATE，实得 %d", err);
    err = g_fn.build_desc(true, &a, buf, 10, &len);
    CHECK(err == ESP_ERR_INVALID_SIZE, "cap=10 应返回 ESP_ERR_INVALID_SIZE，实得 %d", err);
    err = g_fn.build_desc(true, NULL, buf, sizeof(buf), &len);
    CHECK(err == ESP_ERR_INVALID_ARG, "alloc=NULL 应返回 ESP_ERR_INVALID_ARG，实得 %d", err);

    /* ---------- 汇总 ---------- */
    printf("\n=== 结果：PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    if (g_fail) {
        printf("判定：FAIL\n");
        return 1;
    }
    printf("判定：PASS（离线；≠ 真机枚举通过）\n");
    return 0;
}
