#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t tca9554_init(i2c_master_bus_handle_t i2c_bus, uint8_t address);
esp_err_t tca9554_set_pin_mode(uint8_t exio_pin, bool output_mode);
esp_err_t tca9554_write_pin(uint8_t exio_pin, bool high_level);
esp_err_t tca9554_read_pin(uint8_t exio_pin, bool *out_level);
uint8_t tca9554_get_address(void);
