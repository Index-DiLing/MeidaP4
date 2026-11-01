/* 主机侧离线测试专用的最小 tusb_config.h。
 *
 * 目的：让原生 gcc 能直接包含 **上游真实的** `tusb.h` / `usbd.h`，
 * 从而拿到真正的 `TUD_MTP_DESCRIPTOR` 宏与 `TUSB_*` 常量做逐字节比对。
 * 这里刻意不引 esp_tinyusb 的 tusb_config.h（它会拉 sdkconfig.h / esp_assert.h 等 IDF 头）。 */
#pragma once

#define CFG_TUSB_MCU            OPT_MCU_NONE
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MTP             1
#define CFG_TUD_ENDPOINT0_SIZE  64

/* 上游 tusb_mcu.h:749 对"未定义 TUP_DCD_ENDPOINT_MAX 的 MCU"会打 #warning；
   这里显式给默认值 8（它只影响 DCD 内部数组尺寸，本测试不用 DCD）。 */
#define TUP_DCD_ENDPOINT_MAX    8

/* ---- MTP 类：与 esp_tinyusb/include/tusb_config.h:164-217 逐项一致 ----
   （这里是为了让 `class/mtp/mtp_device.h` 能编过，取值必须与设备侧相同，
     否则主机侧的"参考模板"就和设备侧不是一个东西了） */
#define CFG_TUD_MTP_EP_BUFSIZE           4096
#define CFG_TUD_MTP_EP_CONTROL_BUFSIZE   16
#define CFG_TUD_MTP_MAX_STORAGES         1

#define MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64   0x95C1U
#define MTP_OP_ANDROID_SEND_PARTIAL_OBJECT     0x95C2U
#define MTP_OP_ANDROID_TRUNCATE_OBJECT         0x95C3U
#define MTP_OP_ANDROID_BEGIN_EDIT_OBJECT       0x95C4U
#define MTP_OP_ANDROID_END_EDIT_OBJECT         0x95C5U

#define CFG_TUD_MTP_DEVICEINFO_EXTENSIONS   "microsoft.com: 1.0; android.com: 1.0; "
#define CFG_TUD_MTP_DEVICEINFO_SUPPORTED_OPERATIONS \
    MTP_OP_GET_DEVICE_INFO, MTP_OP_OPEN_SESSION, MTP_OP_CLOSE_SESSION, \
    MTP_OP_GET_STORAGE_IDS, MTP_OP_GET_STORAGE_INFO, MTP_OP_GET_NUM_OBJECTS, \
    MTP_OP_GET_OBJECT_HANDLES, MTP_OP_GET_OBJECT_INFO, MTP_OP_GET_OBJECT, \
    MTP_OP_SEND_OBJECT_INFO, MTP_OP_SEND_OBJECT, MTP_OP_DELETE_OBJECT
#define CFG_TUD_MTP_DEVICEINFO_SUPPORTED_EVENTS
#define CFG_TUD_MTP_DEVICEINFO_SUPPORTED_DEVICE_PROPERTIES \
    MTP_DEV_PROP_DEVICE_FRIENDLY_NAME
#define CFG_TUD_MTP_DEVICEINFO_CAPTURE_FORMATS \
    MTP_OBJ_FORMAT_UNDEFINED, MTP_OBJ_FORMAT_ASSOCIATION
#define CFG_TUD_MTP_DEVICEINFO_PLAYBACK_FORMATS \
    MTP_OBJ_FORMAT_UNDEFINED, MTP_OBJ_FORMAT_ASSOCIATION
