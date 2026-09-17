#include "service_voice.h"

#include <stdio.h>
#include <string.h>

#include "app_pages.h"
#include "app_state.h"
#include "app_status.h"
#include "drivers_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "service_audio.h"
#include "voice_backend.h"

static const char *TAG = "service_voice";

enum {
    VOICE_TASK_STACK = 4096,
    VOICE_TASK_PRIORITY = 6,
    VOICE_TASK_CORE = 1,
    VOICE_COMMAND_QUEUE_LEN = 8,
    VOICE_COMMAND_WAIT_MS = 20,
    VOICE_CAPTURE_TIMEOUT_MS = 0,
    VOICE_PLAYBACK_TIMEOUT_MS = 0,
    VOICE_RUNTIME_LOG_PERIOD_MS = 1000,
    VOICE_CAPTURE_FRAME_BYTES = 1280,
    VOICE_PLAYBACK_FRAME_BYTES = 1920,
};

typedef struct {
    service_voice_command_type_t type;
    service_voice_state_t target_state;
} service_voice_command_t;

static bool s_ready;
static service_voice_state_t s_state = SERVICE_VOICE_STATE_IDLE;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_task_handle;
static TickType_t s_session_start_tick;
static TickType_t s_state_enter_tick;
static TickType_t s_last_runtime_log_tick;
static uint32_t s_session_counter;
static uint32_t s_total_input_bytes;
static uint32_t s_total_output_bytes;
static uint32_t s_input_frame_count;
static uint32_t s_output_frame_count;
static uint32_t s_state_change_counter;
static bool s_last_output_silence;
static uint8_t s_capture_buffer[VOICE_CAPTURE_FRAME_BYTES];
static uint8_t s_playback_buffer[VOICE_PLAYBACK_FRAME_BYTES];

static bool is_voice_session_active(service_voice_state_t state);
static esp_err_t post_voice_command(service_voice_command_type_t type, service_voice_state_t target_state);
static esp_err_t init_voice_runtime(void);
static void task_voice_runtime(void *arg);
static void process_voice_runtime_tick(void);
static void log_voice_session_transition(service_voice_state_t previous_state, service_voice_state_t next_state);
static void publish_voice_runtime_state(void);
static void build_voice_runtime_text(char *input_text, size_t input_size, char *output_text, size_t output_size);
static void apply_voice_audio_policy(void);

static void apply_voice_audio_policy(void)
{
    bool active = false;
    bool mic_requested = false;
    bool speaker_requested = false;
    bool speaker_muted = false;
    const char *policy_text = "idle";

    switch (s_state) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        active = true;
        mic_requested = true;
        policy_text = "voice_wake_capture";
        break;
    case SERVICE_VOICE_STATE_STREAMING:
        active = true;
        mic_requested = true;
        policy_text = "voice_stream_capture";
        break;
    case SERVICE_VOICE_STATE_SPEAKING:
        active = true;
        speaker_requested = true;
        speaker_muted = s_last_output_silence;
        policy_text = s_last_output_silence ? "voice_think_hold" : "voice_reply_playback";
        break;
    case SERVICE_VOICE_STATE_ERROR:
    case SERVICE_VOICE_STATE_IDLE:
    default:
        break;
    }

    (void)service_audio_set_request(SERVICE_AUDIO_OWNER_VOICE,
                                    active,
                                    mic_requested,
                                    speaker_requested,
                                    speaker_muted,
                                    policy_text);
}

static void commit_voice_state(service_voice_state_t state)
{
    const service_voice_state_t previous_state = s_state;
    const bool was_active = is_voice_session_active(previous_state);
    const bool is_active = is_voice_session_active(state);

    s_state = state;
    app_state_set_voice_active(is_active);
    apply_voice_audio_policy();
    log_voice_session_transition(previous_state, state);
    publish_voice_runtime_state();

    APP_LOGI(TAG,
             APP_EVT_VOICE_STATE,
             "voice state=%s active=%d previous_active=%d",
             service_voice_state_to_string(state),
             is_active,
             was_active);
}

static void log_voice_session_transition(service_voice_state_t previous_state, service_voice_state_t next_state)
{
    const bool was_active = is_voice_session_active(previous_state);
    const bool is_active = is_voice_session_active(next_state);
    const TickType_t now = xTaskGetTickCount();

    if (previous_state != next_state) {
        s_state_change_counter++;
        s_state_enter_tick = now;
    }

    if (!was_active && is_active) {
        s_session_counter++;
        s_session_start_tick = now;
        s_state_enter_tick = now;
        s_last_runtime_log_tick = now;
        s_total_input_bytes = 0;
        s_total_output_bytes = 0;
        s_input_frame_count = 0;
        s_output_frame_count = 0;
        s_state_change_counter = 1;
        s_last_output_silence = false;
        (void)voice_backend_start_session();
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "voice session #%lu started state=%s",
                 (unsigned long)s_session_counter,
                 service_voice_state_to_string(next_state));
        return;
    }

    if (was_active && !is_active) {
        (void)voice_backend_stop_session();
        const uint32_t session_ms = (uint32_t)(now - s_session_start_tick) * portTICK_PERIOD_MS;
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "voice session #%lu ended from=%s duration=%lums",
                 (unsigned long)s_session_counter,
                 service_voice_state_to_string(previous_state),
                 (unsigned long)session_ms);
    }
}

static void publish_voice_runtime_state(void)
{
    uint32_t session_duration_ms = 0;
    uint32_t state_duration_ms = 0;
    const TickType_t now = xTaskGetTickCount();
    char input_text[VOICE_RUNTIME_TEXT_MAX] = {0};
    char output_text[VOICE_RUNTIME_TEXT_MAX] = {0};

    if (is_voice_session_active(s_state)) {
        session_duration_ms = (uint32_t)(now - s_session_start_tick) * portTICK_PERIOD_MS;
        state_duration_ms = (uint32_t)(now - s_state_enter_tick) * portTICK_PERIOD_MS;
    }

    build_voice_runtime_text(input_text, sizeof(input_text), output_text, sizeof(output_text));

    app_state_set_voice_runtime((uint8_t)s_state,
                                s_session_counter,
                                session_duration_ms,
                                state_duration_ms,
                                s_total_input_bytes,
                                s_total_output_bytes,
                                s_input_frame_count,
                                s_output_frame_count,
                                s_state_change_counter,
                                s_last_output_silence,
                                input_text,
                                output_text);
}

static void build_voice_runtime_text(char *input_text, size_t input_size, char *output_text, size_t output_size)
{
    if (input_text == NULL || input_size == 0U || output_text == NULL || output_size == 0U) {
        return;
    }

    input_text[0] = '\0';
    output_text[0] = '\0';

    switch (s_state) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        snprintf(input_text, input_size, "\"PathFinder\" wake phrase captured.");
        snprintf(output_text, output_size, "Assistant: bringing up the voice session.");
        break;
    case SERVICE_VOICE_STATE_STREAMING:
        if (s_input_frame_count > 0U) {
            snprintf(input_text,
                     input_size,
                     "User: start the dashboard demo, session %lu.",
                     (unsigned long)s_session_counter);
        } else {
            snprintf(input_text, input_size, "User: waiting to speak.");
        }
        snprintf(output_text, output_size, "Assistant: receiving mic frames and building request.");
        break;
    case SERVICE_VOICE_STATE_SPEAKING:
        if (s_last_output_silence) {
            snprintf(input_text, input_size, "User: request latched, waiting for reply.");
            snprintf(output_text, output_size, "Assistant: thinking through a mock response.");
        } else {
            snprintf(input_text, input_size, "User: request complete.");
            snprintf(output_text,
                     output_size,
                     "Assistant: mock reply %lu is streaming to speaker.",
                     (unsigned long)s_session_counter);
        }
        break;
    case SERVICE_VOICE_STATE_ERROR:
        snprintf(input_text, input_size, "User: session interrupted.");
        snprintf(output_text, output_size, "Assistant: backend entered recovery.");
        break;
    case SERVICE_VOICE_STATE_IDLE:
    default:
        if (s_session_counter > 0U) {
            snprintf(input_text,
                     input_size,
                     "User: last request finished in session %lu.",
                     (unsigned long)s_session_counter);
            snprintf(output_text, output_size, "Assistant: mock reply complete, ready for wake.");
        } else {
            snprintf(input_text, input_size, "User: say \"PathFinder\" to start.");
            snprintf(output_text, output_size, "Assistant: idle and ready.");
        }
        break;
    }
}

static service_voice_state_t map_backend_state(voice_backend_state_t state)
{
    switch (state) {
    case VOICE_BACKEND_STATE_WAKE_DETECTED:
        return SERVICE_VOICE_STATE_WAKE_DETECTED;
    case VOICE_BACKEND_STATE_STREAMING:
        return SERVICE_VOICE_STATE_STREAMING;
    case VOICE_BACKEND_STATE_SPEAKING:
        return SERVICE_VOICE_STATE_SPEAKING;
    case VOICE_BACKEND_STATE_ERROR:
        return SERVICE_VOICE_STATE_ERROR;
    case VOICE_BACKEND_STATE_IDLE:
    default:
        return SERVICE_VOICE_STATE_IDLE;
    }
}

static voice_backend_state_t map_service_state(service_voice_state_t state)
{
    switch (state) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return VOICE_BACKEND_STATE_WAKE_DETECTED;
    case SERVICE_VOICE_STATE_STREAMING:
        return VOICE_BACKEND_STATE_STREAMING;
    case SERVICE_VOICE_STATE_SPEAKING:
        return VOICE_BACKEND_STATE_SPEAKING;
    case SERVICE_VOICE_STATE_ERROR:
        return VOICE_BACKEND_STATE_ERROR;
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return VOICE_BACKEND_STATE_IDLE;
    }
}

static void on_backend_state_changed(voice_backend_state_t state, void *user_ctx)
{
    (void)user_ctx;

    if (!s_ready) {
        return;
    }

    commit_voice_state(map_backend_state(state));
}

static esp_err_t init_voice_runtime(void)
{
    const voice_backend_info_t backend_info = voice_backend_get_info();
    const esp_err_t audio_init_err = service_audio_init();
    if (audio_init_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_AUDIO_INIT, "audio policy init failed: %s", esp_err_to_name(audio_init_err));
        return audio_init_err;
    }

    const esp_err_t backend_init_err = voice_backend_init(on_backend_state_changed, NULL);
    if (backend_init_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_VOICE_BACKEND_INIT, "voice backend init failed: %s", esp_err_to_name(backend_init_err));
        return backend_init_err;
    }

    s_state = SERVICE_VOICE_STATE_IDLE;
    s_ready = true;
    commit_voice_state(map_backend_state(voice_backend_get_state()));
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "voice task ready backend=%s integration=%s core=%d priority=%d",
             voice_backend_vendor_to_string(backend_info.vendor),
             voice_backend_integration_to_string(backend_info.integration),
             VOICE_TASK_CORE,
             VOICE_TASK_PRIORITY);
    return ESP_OK;
}

static void task_voice_runtime(void *arg)
{
    (void)arg;

    const esp_err_t init_err = init_voice_runtime();
    if (init_err != ESP_OK) {
        s_ready = false;
        APP_LOGE(TAG, APP_ERR_VOICE_INIT, "voice runtime init failed: %s", esp_err_to_name(init_err));
    }

    while (true) {
        service_voice_command_t command = {0};
        if (xQueueReceive(s_command_queue, &command, pdMS_TO_TICKS(VOICE_COMMAND_WAIT_MS)) != pdTRUE) {
            process_voice_runtime_tick();
            continue;
        }

        if (!s_ready) {
            APP_LOGW(TAG, APP_ERR_VOICE_INIT, "drop voice command type=%d before runtime ready", (int)command.type);
            continue;
        }

        switch (command.type) {
        case SERVICE_VOICE_CMD_SET_STATE:
            (void)voice_backend_inject_state(map_service_state(command.target_state));
            break;
        case SERVICE_VOICE_CMD_RESET_SESSION:
            (void)voice_backend_inject_state(VOICE_BACKEND_STATE_IDLE);
            break;
        default:
            APP_LOGW(TAG, APP_ERR_VOICE_INIT, "unknown voice command type=%d", (int)command.type);
            break;
        }
    }
}

static void process_voice_runtime_tick(void)
{
    const TickType_t now = xTaskGetTickCount();
    const bool session_active = is_voice_session_active(s_state);

    if (!s_ready || !session_active) {
        return;
    }

    size_t bytes_read = 0;
    size_t bytes_written = 0;
    voice_backend_tick_result_t tick_result = {0};

    if (s_state == SERVICE_VOICE_STATE_WAKE_DETECTED || s_state == SERVICE_VOICE_STATE_STREAMING) {
        const esp_err_t read_err = drivers_audio_read_input(s_capture_buffer,
                                                            sizeof(s_capture_buffer),
                                                            &bytes_read,
                                                            VOICE_CAPTURE_TIMEOUT_MS);
        if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            APP_LOGW(TAG, APP_ERR_AUDIO_INIT, "voice capture tick failed: %s", esp_err_to_name(read_err));
        }
    }

    const esp_err_t backend_tick_err = voice_backend_process_audio(s_capture_buffer,
                                                                   bytes_read,
                                                                   s_playback_buffer,
                                                                   sizeof(s_playback_buffer),
                                                                   &tick_result);
    if (backend_tick_err != ESP_OK && backend_tick_err != ESP_ERR_INVALID_STATE) {
        APP_LOGW(TAG, APP_ERR_VOICE_BACKEND_INIT, "voice backend tick failed: %s", esp_err_to_name(backend_tick_err));
    }

    if (tick_result.output_bytes_produced > 0) {
        const esp_err_t write_err = drivers_audio_write_output(s_playback_buffer,
                                                               tick_result.output_bytes_produced,
                                                               &bytes_written,
                                                               VOICE_PLAYBACK_TIMEOUT_MS);
        if (write_err != ESP_OK && write_err != ESP_ERR_TIMEOUT) {
            APP_LOGW(TAG, APP_ERR_AUDIO_INIT, "voice playback tick failed: %s", esp_err_to_name(write_err));
        }
    }

    s_total_input_bytes += (uint32_t)tick_result.input_bytes_consumed;
    s_total_output_bytes += (uint32_t)bytes_written;
    if (tick_result.input_bytes_consumed > 0U) {
        s_input_frame_count++;
    }
    if (bytes_written > 0U) {
        s_output_frame_count++;
    }
    s_last_output_silence = tick_result.output_is_silence;
    apply_voice_audio_policy();
    publish_voice_runtime_state();

    if ((now - s_last_runtime_log_tick) >= pdMS_TO_TICKS(VOICE_RUNTIME_LOG_PERIOD_MS)) {
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "voice runtime session=%lu state=%s captured=%u consumed=%u played=%u silent=%d",
                 (unsigned long)s_session_counter,
                 service_voice_state_to_string(s_state),
                 (unsigned int)bytes_read,
                 (unsigned int)tick_result.input_bytes_consumed,
                 (unsigned int)bytes_written,
                 tick_result.output_is_silence);
        s_last_runtime_log_tick = now;
    }
}

static bool is_voice_session_active(service_voice_state_t state)
{
    switch (state) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
    case SERVICE_VOICE_STATE_STREAMING:
    case SERVICE_VOICE_STATE_SPEAKING:
        return true;
    case SERVICE_VOICE_STATE_IDLE:
    case SERVICE_VOICE_STATE_ERROR:
    default:
        return false;
    }
}

const char *service_voice_state_to_string(service_voice_state_t state)
{
    switch (state) {
    case SERVICE_VOICE_STATE_IDLE:
        return "idle";
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "wake_detected";
    case SERVICE_VOICE_STATE_STREAMING:
        return "streaming";
    case SERVICE_VOICE_STATE_SPEAKING:
        return "speaking";
    case SERVICE_VOICE_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

esp_err_t service_voice_init(void)
{
    if (s_command_queue != NULL && s_task_handle != NULL) {
        return ESP_OK;
    }

    s_command_queue = xQueueCreate(VOICE_COMMAND_QUEUE_LEN, sizeof(service_voice_command_t));
    if (s_command_queue == NULL) {
        APP_LOGE(TAG, APP_ERR_VOICE_INIT, "create voice command queue failed");
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(task_voice_runtime,
                                                       "task_voice",
                                                       VOICE_TASK_STACK,
                                                       NULL,
                                                       VOICE_TASK_PRIORITY,
                                                       &s_task_handle,
                                                       VOICE_TASK_CORE);
    if (created != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        s_task_handle = NULL;
        APP_LOGE(TAG, APP_ERR_VOICE_INIT, "create task_voice failed");
        return ESP_ERR_NO_MEM;
    }

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "voice service bootstrap queued task=task_voice core=%d priority=%d queue_len=%d",
             VOICE_TASK_CORE,
             VOICE_TASK_PRIORITY,
             VOICE_COMMAND_QUEUE_LEN);
    return ESP_OK;
}

bool service_voice_is_ready(void)
{
    return s_ready;
}

service_voice_state_t service_voice_get_state(void)
{
    return s_state;
}

esp_err_t service_voice_set_state(service_voice_state_t state)
{
    return post_voice_command(SERVICE_VOICE_CMD_SET_STATE, state);
}

esp_err_t service_voice_reset_session(void)
{
    return post_voice_command(SERVICE_VOICE_CMD_RESET_SESSION, SERVICE_VOICE_STATE_IDLE);
}

static esp_err_t post_voice_command(service_voice_command_type_t type, service_voice_state_t target_state)
{
    service_voice_command_t command = {
        .type = type,
        .target_state = target_state,
    };

    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        APP_LOGW(TAG, APP_ERR_VOICE_INIT, "voice command queue full type=%d", (int)type);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
