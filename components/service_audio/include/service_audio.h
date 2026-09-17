#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    SERVICE_AUDIO_OWNER_NONE = 0,
    SERVICE_AUDIO_OWNER_SYSTEM,
    SERVICE_AUDIO_OWNER_NAVIGATION,
    SERVICE_AUDIO_OWNER_OBD,
    SERVICE_AUDIO_OWNER_VOICE,
    SERVICE_AUDIO_OWNER_COUNT,
} service_audio_owner_t;

esp_err_t service_audio_init(void);
bool service_audio_is_ready(void);
const char *service_audio_owner_to_string(service_audio_owner_t owner);
esp_err_t service_audio_set_request(service_audio_owner_t owner,
                                    bool active,
                                    bool mic_requested,
                                    bool speaker_requested,
                                    bool speaker_muted,
                                    const char *policy_text);
