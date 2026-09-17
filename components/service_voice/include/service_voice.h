#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    SERVICE_VOICE_STATE_IDLE = 0,
    SERVICE_VOICE_STATE_WAKE_DETECTED,
    SERVICE_VOICE_STATE_STREAMING,
    SERVICE_VOICE_STATE_SPEAKING,
    SERVICE_VOICE_STATE_ERROR,
} service_voice_state_t;

typedef enum {
    SERVICE_VOICE_CMD_SET_STATE = 0,
    SERVICE_VOICE_CMD_RESET_SESSION,
} service_voice_command_type_t;

esp_err_t service_voice_init(void);
bool service_voice_is_ready(void);
service_voice_state_t service_voice_get_state(void);
esp_err_t service_voice_set_state(service_voice_state_t state);
esp_err_t service_voice_reset_session(void);
const char *service_voice_state_to_string(service_voice_state_t state);
