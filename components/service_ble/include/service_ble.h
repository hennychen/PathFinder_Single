#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*service_ble_write_handler_t)(const uint8_t *payload, size_t payload_len);

esp_err_t service_ble_register_bridge_handlers(service_ble_write_handler_t navigation_handler,
                                               service_ble_write_handler_t obd_handler);
esp_err_t service_ble_init(void);
bool service_ble_is_ready(void);
bool service_ble_host_is_active(void);
bool service_ble_host_is_synced(void);
