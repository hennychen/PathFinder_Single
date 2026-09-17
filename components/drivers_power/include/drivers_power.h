#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t battery_voltage_mv;
    uint8_t battery_percent;
    bool calibrated;
} drivers_power_status_t;

esp_err_t drivers_power_init(void);
bool drivers_power_is_ready(void);
esp_err_t drivers_power_read_status(drivers_power_status_t *out_status);
