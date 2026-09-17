#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "obd_protocol.h"

enum {
    SERVICE_OBD_TIMEOUT_MS = 3000,
};

esp_err_t service_obd_init(void);
bool service_obd_is_ready(void);
esp_err_t service_obd_submit_packet(const uint8_t *packet, size_t packet_len);
esp_err_t service_obd_submit_mock_sample(uint16_t speed_kmh,
                                         uint16_t rpm,
                                         int8_t coolant_temp_c,
                                         uint8_t throttle_percent,
                                         uint8_t fuel_percent,
                                         bool connected);
