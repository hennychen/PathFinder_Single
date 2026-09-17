#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t service_attitude_init(void);
bool service_attitude_is_ready(void);
