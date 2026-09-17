#include "service_audio.h"

#include <string.h>

#include "app_state.h"
#include "app_status.h"
#include "drivers_audio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "service_audio";

typedef struct {
    bool active;
    bool mic_requested;
    bool speaker_requested;
    bool speaker_muted;
    char policy_text[AUDIO_POLICY_TEXT_MAX];
} service_audio_request_t;

static bool s_ready;
static service_audio_request_t s_requests[SERVICE_AUDIO_OWNER_COUNT];
static SemaphoreHandle_t s_lock;

static uint8_t owner_priority(service_audio_owner_t owner)
{
    switch (owner) {
    case SERVICE_AUDIO_OWNER_VOICE:
        return 4;
    case SERVICE_AUDIO_OWNER_OBD:
        return 3;
    case SERVICE_AUDIO_OWNER_NAVIGATION:
        return 2;
    case SERVICE_AUDIO_OWNER_SYSTEM:
        return 1;
    case SERVICE_AUDIO_OWNER_NONE:
    default:
        return 0;
    }
}

const char *service_audio_owner_to_string(service_audio_owner_t owner)
{
    switch (owner) {
    case SERVICE_AUDIO_OWNER_SYSTEM:
        return "system";
    case SERVICE_AUDIO_OWNER_NAVIGATION:
        return "navigation";
    case SERVICE_AUDIO_OWNER_OBD:
        return "obd";
    case SERVICE_AUDIO_OWNER_VOICE:
        return "voice";
    case SERVICE_AUDIO_OWNER_NONE:
    default:
        return "none";
    }
}

static void service_audio_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void service_audio_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void publish_audio_state_locked(service_audio_owner_t owner,
                                       bool mic_enabled,
                                       bool speaker_enabled,
                                       bool speaker_muted)
{
    audio_runtime_state_t state = {0};
    const service_audio_request_t *request = &s_requests[owner];

    state.owner_code = (uint8_t)owner;
    state.mic_requested = request->active && request->mic_requested;
    state.speaker_requested = request->active && request->speaker_requested;
    state.mic_enabled = mic_enabled;
    state.speaker_enabled = speaker_enabled;
    state.speaker_muted = request->active && request->speaker_requested && speaker_muted;
    strncpy(state.owner_text, service_audio_owner_to_string(owner), sizeof(state.owner_text) - 1U);
    state.owner_text[sizeof(state.owner_text) - 1U] = '\0';
    strncpy(state.policy_text, request->policy_text, sizeof(state.policy_text) - 1U);
    state.policy_text[sizeof(state.policy_text) - 1U] = '\0';

    app_state_set_audio_state(&state);
}

static esp_err_t apply_resolved_request_locked(void)
{
    service_audio_owner_t owner = SERVICE_AUDIO_OWNER_NONE;
    uint8_t best_priority = 0;
    bool mic_enabled = false;
    bool speaker_enabled = false;
    bool speaker_muted = false;

    for (int i = 1; i < (int)SERVICE_AUDIO_OWNER_COUNT; ++i) {
        const service_audio_request_t *request = &s_requests[i];
        const uint8_t priority = owner_priority((service_audio_owner_t)i);

        if (!request->active || priority < best_priority) {
            continue;
        }

        owner = (service_audio_owner_t)i;
        best_priority = priority;
    }

    if (owner != SERVICE_AUDIO_OWNER_NONE) {
        const service_audio_request_t *request = &s_requests[owner];
        mic_enabled = request->mic_requested;
        speaker_muted = request->speaker_requested && request->speaker_muted;
        speaker_enabled = request->speaker_requested && !request->speaker_muted;
    }

    ESP_RETURN_ON_ERROR(drivers_audio_set_input_enabled(mic_enabled), TAG, "apply mic policy failed");
    ESP_RETURN_ON_ERROR(drivers_audio_set_output_enabled(speaker_enabled), TAG, "apply speaker policy failed");

    publish_audio_state_locked(owner, mic_enabled, speaker_enabled, speaker_muted);
    APP_LOGI(TAG,
             APP_EVT_AUDIO_POLICY,
             "audio owner=%s mic(req=%d en=%d) spk(req=%d en=%d mute=%d) policy=%s",
             service_audio_owner_to_string(owner),
             owner != SERVICE_AUDIO_OWNER_NONE ? s_requests[owner].mic_requested : 0,
             mic_enabled,
             owner != SERVICE_AUDIO_OWNER_NONE ? s_requests[owner].speaker_requested : 0,
             speaker_enabled,
             speaker_muted,
             owner != SERVICE_AUDIO_OWNER_NONE && s_requests[owner].policy_text[0] != '\0'
                 ? s_requests[owner].policy_text
                 : "idle");
    return ESP_OK;
}

esp_err_t service_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(drivers_audio_init(), TAG, "audio driver init failed");
    ESP_RETURN_ON_ERROR(drivers_audio_set_input_enabled(false), TAG, "disable mic by default failed");
    ESP_RETURN_ON_ERROR(drivers_audio_set_output_enabled(false), TAG, "disable speaker by default failed");

    service_audio_lock();
    memset(s_requests, 0, sizeof(s_requests));
    publish_audio_state_locked(SERVICE_AUDIO_OWNER_NONE, false, false, false);
    service_audio_unlock();

    s_ready = true;
    APP_LOGI(TAG, APP_STATUS_OK, "audio policy service ready");
    return ESP_OK;
}

bool service_audio_is_ready(void)
{
    return s_ready;
}

esp_err_t service_audio_set_request(service_audio_owner_t owner,
                                    bool active,
                                    bool mic_requested,
                                    bool speaker_requested,
                                    bool speaker_muted,
                                    const char *policy_text)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (owner <= SERVICE_AUDIO_OWNER_NONE || owner >= SERVICE_AUDIO_OWNER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    service_audio_lock();
    const bool same_request = (s_requests[owner].active == active) &&
                              (s_requests[owner].mic_requested == (active && mic_requested)) &&
                              (s_requests[owner].speaker_requested == (active && speaker_requested)) &&
                              (s_requests[owner].speaker_muted == (active && speaker_requested && speaker_muted)) &&
                              (((policy_text == NULL || policy_text[0] == '\0') && s_requests[owner].policy_text[0] == '\0') ||
                               (policy_text != NULL && strcmp(s_requests[owner].policy_text, policy_text) == 0));
    if (same_request) {
        service_audio_unlock();
        return ESP_OK;
    }

    s_requests[owner].active = active;
    s_requests[owner].mic_requested = active && mic_requested;
    s_requests[owner].speaker_requested = active && speaker_requested;
    s_requests[owner].speaker_muted = active && speaker_requested && speaker_muted;
    if (policy_text != NULL) {
        strncpy(s_requests[owner].policy_text, policy_text, sizeof(s_requests[owner].policy_text) - 1U);
        s_requests[owner].policy_text[sizeof(s_requests[owner].policy_text) - 1U] = '\0';
    } else {
        s_requests[owner].policy_text[0] = '\0';
    }

    if (!active) {
        s_requests[owner].mic_requested = false;
        s_requests[owner].speaker_requested = false;
        s_requests[owner].speaker_muted = false;
        s_requests[owner].policy_text[0] = '\0';
    }

    const esp_err_t err = apply_resolved_request_locked();
    service_audio_unlock();
    return err;
}
