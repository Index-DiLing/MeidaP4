// 主机侧离线测试专用 stub：mtp_backend.c 引用到、但本测试不关心的外部符号。
// ⚠️ 只在 components/mtp_backend/test/host/ 里使用，绝不进固件构建。
#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "sd_storage.h"
#include "tinyusb_mtp.h"
#include "usb_composite.h"
#include "sdkconfig.h"

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                 return "ESP_OK";
    case ESP_FAIL:               return "ESP_FAIL";
    case ESP_ERR_NO_MEM:         return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:    return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:  return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:   return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:      return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:  return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_NOT_IMPLEMENTED:return "ESP_ERR_NOT_IMPLEMENTED";
    default:                     return "ESP_ERR_?";
    }
}

/* ---- sd_storage（真机才需要；主机测试只验证描述符与注册形态） ---- */
esp_err_t sd_storage_init(void) { return ESP_OK; }
bool sd_storage_is_mounted(void) { return true; }
const char *sd_storage_mount_point(void) { return CONFIG_SD_STORAGE_MOUNT_POINT; }

/* ---- esp_tinyusb 的 MTP 类驱动 ---- */
esp_err_t tinyusb_mtp_install_driver(const tinyusb_mtp_driver_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t tinyusb_mtp_register_storage(const tinyusb_mtp_storage_config_t *config,
                                      tinyusb_mtp_storage_handle_t *handle)
{
    (void)config;
    if (handle != NULL) {
        *handle = (tinyusb_mtp_storage_handle_t)1;
    }
    return ESP_OK;
}

/* ---- I1 复合层 ---- */
esp_err_t usb_composite_register(const usb_function_t *fn)
{
    (void)fn;
    return ESP_OK;
}
