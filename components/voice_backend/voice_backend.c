#include "voice_backend.h"

#include <string.h>
#include <stdint.h>

#include "app_status.h"

static const char *TAG = "voice_backend";

enum {
    VOICE_BACKEND_INPUT_SAMPLE_RATE_HZ = 16000,
    VOICE_BACKEND_OUTPUT_SAMPLE_RATE_HZ = 24000,
    VOICE_BACKEND_STREAM_TRIGGER_BYTES = 1280,
    VOICE_BACKEND_REPLY_TRIGGER_BYTES = 5120,
    VOICE_BACKEND_THINK_TICKS = 4,
    VOICE_BACKEND_SPEAK_TICKS = 10,
};

static bool s_ready;
static voice_backend_state_t s_state = VOICE_BACKEND_STATE_IDLE;
static voice_backend_state_callback_t s_state_callback;
static void *s_state_callback_ctx;
static bool s_session_active;
static uint32_t s_session_input_bytes;
static uint32_t s_session_stream_ticks;
static uint32_t s_session_speaking_ticks;
static uint32_t s_mock_wave_phase;

static const voice_backend_info_t k_backend_info = {
    .vendor = VOICE_BACKEND_VENDOR_XIAOZHI_ESP32,
    .integration = VOICE_BACKEND_INTEGRATION_COMPONENT,
    .input_sample_rate_hz = VOICE_BACKEND_INPUT_SAMPLE_RATE_HZ,
    .output_sample_rate_hz = VOICE_BACKEND_OUTPUT_SAMPLE_RATE_HZ,
};

static void reset_backend_session_metrics(void)
{
    s_session_input_bytes = 0;
    s_session_stream_ticks = 0;
    s_session_speaking_ticks = 0;
    s_mock_wave_phase = 0;
}

static void publish_backend_state(voice_backend_state_t state)
{
    if (s_state == state) {
        return;
    }

    s_state = state;
    APP_LOGI(TAG, APP_EVT_VOICE_BACKEND_STATE, "backend state=%s", voice_backend_state_to_string(state));

    if (s_state_callback != NULL) {
        s_state_callback(state, s_state_callback_ctx);
    }
}

static void fill_mock_reply_audio(void *output_buffer, size_t output_bytes)
{
    uint8_t *buffer = (uint8_t *)output_buffer;

    if (buffer == NULL || output_bytes < 4U) {
        return;
    }

    for (size_t i = 0; i + 3U < output_bytes; i += 4U) {
        const int16_t sample = (int16_t)(((int32_t)(s_mock_wave_phase % 32U) - 16) * 512);
        buffer[i] = (uint8_t)(sample & 0xFF);
        buffer[i + 1U] = (uint8_t)((sample >> 8) & 0xFF);
        buffer[i + 2U] = buffer[i];
        buffer[i + 3U] = buffer[i + 1U];
        s_mock_wave_phase++;
    }
}

const char *voice_backend_state_to_string(voice_backend_state_t state)
{
    switch (state) {
    case VOICE_BACKEND_STATE_IDLE:
        return "idle";
    case VOICE_BACKEND_STATE_WAKE_DETECTED:
        return "wake_detected";
    case VOICE_BACKEND_STATE_STREAMING:
        return "streaming";
    case VOICE_BACKEND_STATE_SPEAKING:
        return "speaking";
    case VOICE_BACKEND_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *voice_backend_vendor_to_string(voice_backend_vendor_t vendor)
{
    switch (vendor) {
    case VOICE_BACKEND_VENDOR_XIAOZHI_ESP32:
        return "xiaozhi-esp32";
    case VOICE_BACKEND_VENDOR_NONE:
    default:
        return "none";
    }
}

const char *voice_backend_integration_to_string(voice_backend_integration_t integration)
{
    switch (integration) {
    case VOICE_BACKEND_INTEGRATION_COMPONENT:
        return "component";
    case VOICE_BACKEND_INTEGRATION_SUBMODULE:
        return "submodule";
    default:
        return "unknown";
    }
}

esp_err_t voice_backend_init(voice_backend_state_callback_t state_callback, void *user_ctx)
{
    s_state_callback = state_callback;
    s_state_callback_ctx = user_ctx;
    s_state = VOICE_BACKEND_STATE_IDLE;
    s_session_active = false;
    reset_backend_session_metrics();
    s_ready = true;

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "backend selected vendor=%s integration=%s input=%uHz output=%uHz",
             voice_backend_vendor_to_string(k_backend_info.vendor),
             voice_backend_integration_to_string(k_backend_info.integration),
             (unsigned int)k_backend_info.input_sample_rate_hz,
             (unsigned int)k_backend_info.output_sample_rate_hz);
    return ESP_OK;
}

bool voice_backend_is_ready(void)
{
    return s_ready;
}

voice_backend_info_t voice_backend_get_info(void)
{
    return k_backend_info;
}

voice_backend_state_t voice_backend_get_state(void)
{
    return s_state;
}

esp_err_t voice_backend_inject_state(voice_backend_state_t state)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (state == VOICE_BACKEND_STATE_IDLE) {
        reset_backend_session_metrics();
    }

    publish_backend_state(state);

    return ESP_OK;
}

esp_err_t voice_backend_start_session(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_session_active = true;
    reset_backend_session_metrics();
    APP_LOGI(TAG, APP_STATUS_OK, "backend session started");
    return ESP_OK;
}

esp_err_t voice_backend_stop_session(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_session_active = false;
    reset_backend_session_metrics();
    APP_LOGI(TAG, APP_STATUS_OK, "backend session stopped");
    return ESP_OK;
}

esp_err_t voice_backend_process_audio(const void *input_buffer,
                                      size_t input_size,
                                      void *output_buffer,
                                      size_t output_capacity,
                                      voice_backend_tick_result_t *out_result)
{
    if (!s_ready || !s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));

    switch (s_state) {
    case VOICE_BACKEND_STATE_WAKE_DETECTED:
        if (input_buffer != NULL && input_size > 0) {
            out_result->input_bytes_consumed = input_size;
            s_session_input_bytes += (uint32_t)input_size;
            if (s_session_input_bytes >= VOICE_BACKEND_STREAM_TRIGGER_BYTES) {
                publish_backend_state(VOICE_BACKEND_STATE_STREAMING);
            }
        }
        break;
    case VOICE_BACKEND_STATE_STREAMING:
        if (input_buffer != NULL && input_size > 0) {
            out_result->input_bytes_consumed = input_size;
            s_session_input_bytes += (uint32_t)input_size;
            s_session_stream_ticks++;
        }
        if (s_session_input_bytes >= VOICE_BACKEND_REPLY_TRIGGER_BYTES || s_session_stream_ticks >= 4U) {
            s_session_speaking_ticks = 0;
            publish_backend_state(VOICE_BACKEND_STATE_SPEAKING);
        }
        break;
    case VOICE_BACKEND_STATE_SPEAKING:
        if (output_buffer != NULL && output_capacity > 0) {
            const size_t bytes_to_emit = output_capacity;
            if (s_session_speaking_ticks < VOICE_BACKEND_THINK_TICKS) {
                memset(output_buffer, 0, bytes_to_emit);
                out_result->output_is_silence = true;
            } else {
                fill_mock_reply_audio(output_buffer, bytes_to_emit);
                out_result->output_is_silence = false;
            }

            out_result->output_bytes_produced = bytes_to_emit;
            s_session_speaking_ticks++;

            if (s_session_speaking_ticks >= (VOICE_BACKEND_THINK_TICKS + VOICE_BACKEND_SPEAK_TICKS)) {
                publish_backend_state(VOICE_BACKEND_STATE_IDLE);
                reset_backend_session_metrics();
            }
        }
        break;
    case VOICE_BACKEND_STATE_IDLE:
    case VOICE_BACKEND_STATE_ERROR:
    default:
        break;
    }

    return ESP_OK;
}
