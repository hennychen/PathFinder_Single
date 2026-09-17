#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t service_power_init(void);
bool service_power_is_ready(void);
