#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "navigation_protocol.h"

enum {
    SERVICE_NAVIGATION_TIMEOUT_MS = 10000,
};

esp_err_t service_navigation_init(void);
bool service_navigation_is_ready(void);
bool service_navigation_ble_stack_is_active(void);
esp_err_t service_navigation_submit_packet(const uint8_t *packet, size_t packet_len);
esp_err_t service_navigation_submit_mock_packet(navigation_turn_type_t turn_type,
                                                uint16_t step_distance_m,
                                                uint16_t total_distance_m,
                                                uint16_t remain_time_s,
                                                const char *road_name);
