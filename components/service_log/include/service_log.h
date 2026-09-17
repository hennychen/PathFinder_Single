#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t service_log_init(void);
bool service_log_is_ready(void);
const char *service_log_get_path(void);
