// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 主机侧离线测试 —— MTP 描述符片段 vs TinyUSB 自己的 TUD_MTP_DESCRIPTOR
//
//   生产者：WS-storage。写作用域：components/mtp_backend/test/host/**
//   跑法：  pwsh -File components/mtp_backend/test/host/run.ps1      （原生 gcc，无 IDF、无硬件）
//
// 为什么要有它：
//   设备不可用，而"我拼出来的 30 字节描述符片段与上游认可的形状是否逐字节相同"是全套 MTP
//   里最容易写错、又最难在主机侧观察的一环。做法是：**包含真实的 `tusb.h`**，用上游自己的
//   `TUD_MTP_DESCRIPTOR` 宏生成参考字节，再拿固件里**同一份源码**（直接 `#include` 实现文件，
//   不复制粘贴）生成的片段做 memcmp。任何结构/顺序/长度/MPS 的改动都会在这里炸。
//
// ⚠️ 它证明的是"描述符片段形状正确"，**不是** MTP 端到端可用（那只在真机上能验）。

#include <stdio.h>
#include <string.h>

#include "tusb.h"            // 上游真实的 TUD_MTP_DESCRIPTOR / TUD_MTP_DESC_LEN / TUSB_*
#include "usb_composite.h"   // I1 契约的 usb_fn_alloc_t

/* 直接包含被测实现：静态函数可见，且编的就是固件里那份源码 */
#include "../../mtp_backend.c"

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) {                                                   \
            s_pass++;                                                 \
        } else {                                                      \
            s_fail++;                                                 \
            printf("  FAIL: " __VA_ARGS__);                           \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void dump(const char *what, const uint8_t *b, size_t n)
{
    printf("  %s: ", what);
    for (size_t i = 0; i < n; i++) {
        printf("%02X ", b[i]);
    }
    printf("\n");
}

/* ===========================================================================
 * 用例表：alloc 里写的号与期望片段里写的号必须是**同一组字面量**（这是测试的输入，
 * 不是实现里的硬编码）。第三个用例故意用一组"偏离冻结表"的号，用来证明实现真的
 * 是照着 alloc 生成、而不是把 EP1/EP2 写死了。
 * ========================================================================= */
typedef struct {
    const char *name;
    usb_fn_alloc_t alloc;
    uint8_t hs[TUD_MTP_DESC_LEN];
    uint8_t fs[TUD_MTP_DESC_LEN];
} mtp_case_t;

/* ⚠️ 宏参数名**不能**叫 itf/str/... —— 会和 `.itf = ...` 这类字段名撞车，
   预处理会把字段名一起替换掉（已经踩过一次）。 */
#define CASE_ALLOC(_iface, _stridx, _evt, _out, _in)                      \
    {                                                                     \
        .itf = { [0] = (_iface) }, .itf_count = 1, .itf_str_idx = (_stridx), \
        .ep_in = { [0] = (_evt), [1] = (_in) }, .ep_in_count = 2,         \
        .ep_out = { [0] = (_out) }, .ep_out_count = 1,                    \
    }

static const mtp_case_t s_cases[] = {
    {
        .name = "frozen table: IF0 / EP1 int IN / EP2 bulk IN+OUT",
        .alloc = CASE_ALLOC(0, 5, 0x81, 0x02, 0x82),
        .hs = { TUD_MTP_DESCRIPTOR(0, 5, 0x81, 64, 1, 0x02, 0x82, 512) },
        .fs = { TUD_MTP_DESCRIPTOR(0, 5, 0x81, 64, 1, 0x02, 0x82, 64) },
    },
    {
        .name = "deviating numbers (proves nothing is hardcoded): IF2 / EP3 / EP5",
        .alloc = CASE_ALLOC(2, 9, 0x83, 0x05, 0x85),
        .hs = { TUD_MTP_DESCRIPTOR(2, 9, 0x83, 64, 1, 0x05, 0x85, 512) },
        .fs = { TUD_MTP_DESCRIPTOR(2, 9, 0x83, 64, 1, 0x05, 0x85, 64) },
    },
    {
        .name = "runtime self-check placeholder set",
        .alloc = CASE_ALLOC(MTP_REF_ITF, MTP_REF_STR, MTP_REF_EVT_IN, MTP_REF_BULK_OUT, MTP_REF_BULK_IN),
        .hs = { TUD_MTP_DESCRIPTOR(MTP_REF_ITF, MTP_REF_STR, MTP_REF_EVT_IN, MTP_REF_MPS_EVT, 1,
                                   MTP_REF_BULK_OUT, MTP_REF_BULK_IN, MTP_REF_MPS_HS) },
        .fs = { TUD_MTP_DESCRIPTOR(MTP_REF_ITF, MTP_REF_STR, MTP_REF_EVT_IN, MTP_REF_MPS_EVT, 1,
                                   MTP_REF_BULK_OUT, MTP_REF_BULK_IN, MTP_REF_MPS_FS) },
    },
};

static void run_case(const mtp_case_t *c)
{
    printf("case: %s\n", c->name);

    for (int hs = 0; hs < 2; hs++) {
        const char *speed = hs ? "HS" : "FS";
        const uint8_t *ref = hs ? c->hs : c->fs;
        const uint16_t want_bulk_mps = hs ? MTP_EP_BULK_MPS_HS : MTP_EP_BULK_MPS_FS;

        uint8_t got[64];
        size_t len = 0;
        memset(got, 0xAA, sizeof(got));

        esp_err_t err = mtp_build_desc(hs != 0, &c->alloc, got, sizeof(got), &len);
        CHECK(err == ESP_OK, "%s: build_desc -> %s", speed, esp_err_to_name(err));
        CHECK(len == (size_t)TUD_MTP_DESC_LEN, "%s: len=%u, want %u",
              speed, (unsigned)len, (unsigned)TUD_MTP_DESC_LEN);
        if (err != ESP_OK || len != (size_t)TUD_MTP_DESC_LEN) {
            continue;
        }

        const bool same = (memcmp(got, ref, len) == 0);
        CHECK(same, "%s: fragment != TUD_MTP_DESCRIPTOR", speed);
        if (!same) {
            dump("got", got, len);
            dump("ref", ref, len);
        }

        /* 结构语义再钉一遍（即使 memcmp 过了，失败信息也更可读） */
        CHECK(got[0] == 9 && got[1] == TUSB_DESC_INTERFACE, "%s: interface desc header", speed);
        CHECK(got[2] == c->alloc.itf[0], "%s: bInterfaceNumber 必须来自 alloc", speed);
        CHECK(got[3] == 0, "%s: bAlternateSetting 必须为 0", speed);
        CHECK(got[4] == 3, "%s: bNumEndpoints 必须是 3（mtpd_open() 硬要求）", speed);
        CHECK(got[5] == TUSB_CLASS_IMAGE && got[6] == (uint8_t)MTP_SUBCLASS_STILL_IMAGE &&
                  got[7] == (uint8_t)MTP_PROTOCOL_PIMA_15470,
              "%s: 接口类必须是 06/01/01", speed);
        CHECK(got[8] == c->alloc.itf_str_idx, "%s: iInterface 必须来自 alloc", speed);

        CHECK(got[9] == 7 && got[10] == TUSB_DESC_ENDPOINT, "%s: 事件端点描述符头", speed);
        CHECK(got[11] == c->alloc.ep_in[0], "%s: 事件端点号必须来自 alloc", speed);
        CHECK(got[12] == TUSB_XFER_INTERRUPT, "%s: 第一个端点必须是 interrupt IN", speed);
        CHECK(rd16(&got[13]) == 64, "%s: 事件端点 MPS 必须 64", speed);
        CHECK(got[15] == 1, "%s: 事件端点 bInterval 必须 1 ms", speed);

        CHECK(got[16] == 7 && got[18] == c->alloc.ep_out[0], "%s: bulk OUT 端点号必须来自 alloc", speed);
        CHECK(got[19] == TUSB_XFER_BULK, "%s: 第二个端点必须是 bulk OUT", speed);
        CHECK(rd16(&got[20]) == want_bulk_mps, "%s: bulk OUT MPS=%u, want %u",
              speed, rd16(&got[20]), want_bulk_mps);
        CHECK(got[22] == 0, "%s: bulk OUT bInterval 必须 0", speed);

        CHECK(got[23] == 7 && got[25] == c->alloc.ep_in[1], "%s: bulk IN 端点号必须来自 alloc", speed);
        CHECK(got[26] == TUSB_XFER_BULK, "%s: 第三个端点必须是 bulk IN", speed);
        CHECK(rd16(&got[27]) == want_bulk_mps, "%s: bulk IN MPS=%u, want %u",
              speed, rd16(&got[27]), want_bulk_mps);
        CHECK(got[29] == 0, "%s: bulk IN bInterval 必须 0", speed);
        CHECK((got[11] & 0x7F) != (got[18] & 0x7F), "%s: 事件端与数据端点不得同号", speed);
    }
}

static void run_negative(void)
{
    printf("case: error paths\n");

    const usb_fn_alloc_t good = (usb_fn_alloc_t)CASE_ALLOC(0, 5, 0x81, 0x02, 0x82);
    uint8_t buf[64];
    size_t len = 1234;

    CHECK(mtp_build_desc(true, NULL, buf, sizeof(buf), &len) == ESP_ERR_INVALID_ARG,
          "alloc=NULL 应报 ESP_ERR_INVALID_ARG");
    len = 1234;
    CHECK(mtp_build_desc(true, &good, buf, 8, &len) == ESP_ERR_INVALID_SIZE,
          "cap 不足应报 ESP_ERR_INVALID_SIZE");
    CHECK(len == 0, "失败时 out_len 必须写 0（实际 %u）", (unsigned)len);

    usb_fn_alloc_t few_in = good;
    few_in.ep_in_count = 1;
    CHECK(mtp_build_desc(true, &few_in, buf, sizeof(buf), &len) == ESP_ERR_INVALID_STATE,
          "ep_in_count=1 应报 ESP_ERR_INVALID_STATE");

    usb_fn_alloc_t no_out = good;
    no_out.ep_out_count = 0;
    CHECK(mtp_build_desc(true, &no_out, buf, sizeof(buf), &len) == ESP_ERR_INVALID_STATE,
          "ep_out_count=0 应报 ESP_ERR_INVALID_STATE");

    usb_fn_alloc_t out_has_dir_bit = good;
    out_has_dir_bit.ep_out[0] = (uint8_t)(0x80 | 0x02);
    CHECK(mtp_build_desc(true, &out_has_dir_bit, buf, sizeof(buf), &len) == ESP_ERR_INVALID_STATE,
          "OUT 端点带方向位应报 ESP_ERR_INVALID_STATE");

    usb_fn_alloc_t in_without_dir_bit = good;
    in_without_dir_bit.ep_in[1] = 0x02;
    CHECK(mtp_build_desc(true, &in_without_dir_bit, buf, sizeof(buf), &len) == ESP_ERR_INVALID_STATE,
          "IN 端点缺方向位应报 ESP_ERR_INVALID_STATE");
}

static void run_registration_shape(void)
{
    printf("case: I1 registration shape (usb_function_t)\n");

    CHECK(s_mtp_fn.itf_count == 1, "itf_count 必须为 1");
    CHECK(s_mtp_fn.use_iad == false, "单接口功能不得加 IAD");
    CHECK(s_mtp_fn.is_vendor == false, "MTP 不是 vendor 类功能");
    CHECK(s_mtp_fn.wcid == false, "MTP 不发 WCID（Windows 自带 wpdmtp.sys）");
    CHECK(s_mtp_fn.wcid_compatible_id == NULL && s_mtp_fn.wcid_device_interface_guid == NULL,
          "wcid 相关字段必须为 NULL");
    CHECK(s_mtp_fn.on_out == NULL, "on_out 必须为 NULL（数据面归 esp_tinyusb 的 MTP 类驱动）");
    CHECK(s_mtp_fn.on_control == NULL, "MTP 不处理控制请求");
    CHECK(s_mtp_fn.build_desc == mtp_build_desc, "build_desc 必须挂上");

    CHECK(s_mtp_fn.ep_in_count == 2 && s_mtp_fn.ep_out_count == 1, "必须是 2 IN / 1 OUT");
    CHECK(s_mtp_fn.ep_in_req[0].xfer == USB_EP_XFER_INTERRUPT && s_mtp_fn.ep_in_req[0].interval == 1,
          "第 0 个 IN = interrupt，interval=1");
    CHECK(s_mtp_fn.ep_in_req[0].mps_hs == 64 && s_mtp_fn.ep_in_req[0].mps_fs == 64, "事件端点 64/64");
    CHECK(s_mtp_fn.ep_in_req[1].xfer == USB_EP_XFER_BULK && s_mtp_fn.ep_out_req[0].xfer == USB_EP_XFER_BULK,
          "数据端点必须是 bulk");
    CHECK(s_mtp_fn.ep_in_req[1].mps_hs == 512 && s_mtp_fn.ep_in_req[1].mps_fs == 64, "数据 IN 512/64");
    CHECK(s_mtp_fn.ep_out_req[0].mps_hs == 512 && s_mtp_fn.ep_out_req[0].mps_fs == 64, "数据 OUT 512/64");
    CHECK(s_mtp_fn.ep_in_req[1].interval == 0 && s_mtp_fn.ep_out_req[0].interval == 0,
          "bulk 的 bInterval 必须 0");

    CHECK(s_mtp_fn.ep_out_same_num_as_in[0] == 1, "OUT[0] 必须复用 IN[1] 的号码（EP2）");
    for (int i = 1; i < USB_COMPOSITE_MAX_EP_PER_DIR; i++) {
        CHECK(s_mtp_fn.ep_out_same_num_as_in[i] == USB_EP_NO_PAIR,
              "配对表第 %d 项必须是 USB_EP_NO_PAIR（0xFF）", i);
    }

    CHECK(s_mtp_fn.itf_string != NULL && strcmp(s_mtp_fn.itf_string, "MeidaP4 SD Card") == 0,
          "iInterface 必须是 MeidaP4 SD Card");
    CHECK(s_mtp_fn.name != NULL && strcmp(s_mtp_fn.name, "mtp") == 0, "功能名必须是 mtp");
}

int main(void)
{
    printf("=== MeidaP4 · MTP descriptor fragment vs TinyUSB TUD_MTP_DESCRIPTOR ===\n");
    printf("    TUD_MTP_DESC_LEN=%d  TUSB_CLASS_IMAGE=0x%02X  XFER_INTERRUPT=%u  XFER_BULK=%u\n",
           (int)TUD_MTP_DESC_LEN, (unsigned)TUSB_CLASS_IMAGE,
           (unsigned)TUSB_XFER_INTERRUPT, (unsigned)TUSB_XFER_BULK);
    printf("    (offline host test: no IDF, no hardware)\n\n");

    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        run_case(&s_cases[i]);
    }
    run_negative();
    run_registration_shape();

    printf("case: mtp_backend_desc_selfcheck()（固件启动时跑的那一个）\n");
    CHECK(mtp_backend_desc_selfcheck(), "mtp_backend_desc_selfcheck() 必须返回 true");

    printf("\n=== %d passed, %d failed ===\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
