#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VOICE_BACKEND_VENDOR_NONE = 0,
    VOICE_BACKEND_VENDOR_XIAOZHI_ESP32,
} voice_backend_vendor_t;

typedef enum {
    VOICE_BACKEND_INTEGRATION_COMPONENT = 0,
    VOICE_BACKEND_INTEGRATION_SUBMODULE,
} voice_backend_integration_t;

typedef enum {
    VOICE_BACKEND_STATE_IDLE = 0,
    VOICE_BACKEND_STATE_WAKE_DETECTED,
    VOICE_BACKEND_STATE_STREAMING,
    VOICE_BACKEND_STATE_SPEAKING,
    VOICE_BACKEND_STATE_ERROR,
} voice_backend_state_t;

typedef void (*voice_backend_state_callback_t)(voice_backend_state_t state, void *user_ctx);

typedef struct {
    voice_backend_vendor_t vendor;
    voice_backend_integration_t integration;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
} voice_backend_info_t;

typedef struct {
    size_t input_bytes_consumed;
    size_t output_bytes_produced;
    bool output_is_silence;
} voice_backend_tick_result_t;

esp_err_t voice_backend_init(voice_backend_state_callback_t state_callback, void *user_ctx);
bool voice_backend_is_ready(void);
voice_backend_info_t voice_backend_get_info(void);
voice_backend_state_t voice_backend_get_state(void);
const char *voice_backend_state_to_string(voice_backend_state_t state);
const char *voice_backend_vendor_to_string(voice_backend_vendor_t vendor);
const char *voice_backend_integration_to_string(voice_backend_integration_t integration);
esp_err_t voice_backend_inject_state(voice_backend_state_t state);
esp_err_t voice_backend_start_session(void);
esp_err_t voice_backend_stop_session(void);
esp_err_t voice_backend_process_audio(const void *input_buffer,
                                      size_t input_size,
                                      void *output_buffer,
                                      size_t output_capacity,
                                      voice_backend_tick_result_t *out_result);
