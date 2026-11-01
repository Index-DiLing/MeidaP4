/* 离线自检用的最小 esp_netif.h 替身：只提供 usb_net.h / usb_net_priv.h 需要的类型。
 * ⚠️ 这不是真 IDF 头；测试只覆盖**描述符字节**，不覆盖 netif 行为。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct esp_netif esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;
