#include "obd_provider.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "obd_adapter_profile.h"
#include "app_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_result.h"
#include "obd.h"
#include "service_ble.h"

static const char *TAG = "obd_provider";

enum {
    OBD_PROVIDER_CONNECT_RETRY_MS = 3000,
    OBD_PROVIDER_POLL_INTERVAL_MS = 200,
};

typedef struct {
    bool initialized;
    bool connected;
    size_t next_pid_index;
    TickType_t last_connect_attempt_tick;
    TickType_t last_poll_tick;
    obd_provider_sample_t sample;
} obd_provider_runtime_t;

static obd_provider_runtime_t s_runtime;

static const uint8_t s_pid_schedule[] = {
    OBD_PID_ENGINE_RPM,
    OBD_PID_VEHICLE_SPEED,
    OBD_PID_COOLANT_TEMP,
    OBD_PID_THROTTLE_POS,
    OBD_PID_FUEL_LEVEL,
};

typedef esp_err_t (*obd_provider_init_fn_t)(void);
typedef esp_err_t (*obd_provider_poll_fn_t)(obd_provider_sample_t *out_sample, bool *out_has_update);
typedef const char *(*obd_provider_name_fn_t)(void);
typedef void (*obd_provider_diag_fn_t)(obd_provider_diagnostics_t *out_diag);

typedef struct {
    obd_provider_init_fn_t init;
    obd_provider_poll_fn_t poll;
    obd_provider_name_fn_t name;
    obd_provider_diag_fn_t get_diag;
} obd_provider_backend_vtable_t;

static esp_err_t backend_esp_obd_ii_init(void);
static esp_err_t backend_esp_obd_ii_poll(obd_provider_sample_t *out_sample, bool *out_has_update);
static const char *backend_esp_obd_ii_name(void);
static void backend_esp_obd_ii_get_diag(obd_provider_diagnostics_t *out_diag);

esp_err_t obd_provider_service_ble_elm327_init(void);
esp_err_t obd_provider_service_ble_elm327_poll(obd_provider_sample_t *out_sample, bool *out_has_update);
const char *obd_provider_service_ble_elm327_name(void);
void obd_provider_service_ble_elm327_get_diag(obd_provider_diagnostics_t *out_diag);

static const obd_provider_backend_vtable_t k_esp_obd_ii_backend = {
    .init = backend_esp_obd_ii_init,
    .poll = backend_esp_obd_ii_poll,
    .name = backend_esp_obd_ii_name,
    .get_diag = backend_esp_obd_ii_get_diag,
};

static const obd_provider_backend_vtable_t k_service_ble_elm327_backend = {
    .init = obd_provider_service_ble_elm327_init,
    .poll = obd_provider_service_ble_elm327_poll,
    .name = obd_provider_service_ble_elm327_name,
    .get_diag = obd_provider_service_ble_elm327_get_diag,
};

static const obd_provider_backend_vtable_t *s_backend = &k_esp_obd_ii_backend;
static obd_provider_diagnostics_t s_diag;

static void set_diag_text(char *buffer, size_t buffer_size, const char *text);

static void set_diag_status(obd_provider_diagnostics_t *diag, obd_provider_status_code_t status_code)
{
    if (diag == NULL) {
        return;
    }

    diag->status_code = (uint8_t)status_code;
    set_diag_text(diag->stage, sizeof(diag->stage), obd_provider_status_code_to_string(status_code));
}

static void set_diag_text(char *buffer, size_t buffer_size, const char *text)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    if (text == NULL) {
        buffer[0] = '\0';
        return;
    }

    strncpy(buffer, text, buffer_size - 1U);
    buffer[buffer_size - 1U] = '\0';
}

static uint16_t float_to_u16(float value)
{
    if (value <= 0.0f) {
        return 0;
    }

    if (value >= 65535.0f) {
        return 65535U;
    }

    return (uint16_t)lroundf(value);
}

static uint8_t float_to_percent(float value)
{
    if (value <= 0.0f) {
        return 0;
    }

    if (value >= 100.0f) {
        return 100U;
    }

    return (uint8_t)lroundf(value);
}

static int8_t float_to_temp(float value)
{
    if (value <= -128.0f) {
        return -128;
    }

    if (value >= 127.0f) {
        return 127;
    }

    return (int8_t)lroundf(value);
}

static bool update_sample_from_pid(uint8_t pid, float value, obd_provider_sample_t *sample)
{
    if (sample == NULL) {
        return false;
    }

    switch (pid) {
    case OBD_PID_ENGINE_RPM:
        sample->rpm = float_to_u16(value);
        return true;
    case OBD_PID_VEHICLE_SPEED:
        sample->speed_kmh = float_to_u16(value);
        return true;
    case OBD_PID_COOLANT_TEMP:
        sample->coolant_temp_c = float_to_temp(value);
        return true;
    case OBD_PID_THROTTLE_POS:
        sample->throttle_percent = float_to_percent(value);
        return true;
    case OBD_PID_FUEL_LEVEL:
        sample->fuel_percent = float_to_percent(value);
        return true;
    default:
        return false;
    }
}

static void mark_disconnected(void)
{
    s_runtime.connected = false;
    s_runtime.sample.connected = false;
    s_diag.connected = false;
}

static esp_err_t ensure_provider_connected(void)
{
    const TickType_t now = xTaskGetTickCount();

    if (s_runtime.connected && obd_is_connected()) {
        return ESP_OK;
    }

    if ((now - s_runtime.last_connect_attempt_tick) < pdMS_TO_TICKS(OBD_PROVIDER_CONNECT_RETRY_MS)) {
        return ESP_OK;
    }

    s_runtime.last_connect_attempt_tick = now;
    s_diag.connect_attempt_count++;
    set_diag_status(&s_diag, OBD_PROVIDER_STATUS_CONNECTING);
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), "Opening esp_obd_ii link");
    const hal_result_t begin_result = obd_begin();
    if (begin_result == HAL_OK && obd_is_connected()) {
        const char *link_name = obd_active_link_name();
        s_runtime.connected = true;
        s_runtime.sample.connected = true;
        s_diag.connected = true;
        set_diag_status(&s_diag, OBD_PROVIDER_STATUS_LIVE);
        set_diag_text(s_diag.detail, sizeof(s_diag.detail), link_name != NULL ? link_name : "esp_obd_ii link ready");
        ESP_LOGI(TAG, "esp_obd_ii connected via %s", link_name != NULL ? link_name : "unknown");
        return ESP_OK;
    }

    mark_disconnected();
    s_diag.failure_count++;
    set_diag_status(&s_diag, OBD_PROVIDER_STATUS_CONNECT_FAIL);
    snprintf(s_diag.detail, sizeof(s_diag.detail), "obd_begin rc=%d", (int)begin_result);
    ESP_LOGW(TAG, "esp_obd_ii begin failed result=%d", (int)begin_result);
    return ESP_OK;
}

static esp_err_t backend_esp_obd_ii_init(void)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    memset(&s_diag, 0, sizeof(s_diag));
    s_runtime.initialized = true;
    set_diag_text(s_diag.backend_name, sizeof(s_diag.backend_name), "esp_obd_ii");
    set_diag_status(&s_diag, OBD_PROVIDER_STATUS_IDLE);
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), "Waiting for esp_obd_ii open");
    return ESP_OK;
}

static esp_err_t backend_esp_obd_ii_poll(obd_provider_sample_t *out_sample, bool *out_has_update)
{
    float value = 0.0f;
    int read_result = 0;
    const TickType_t now = xTaskGetTickCount();
    const uint8_t pid = s_pid_schedule[s_runtime.next_pid_index];

    if (out_sample == NULL || out_has_update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_has_update = false;

    if (!s_runtime.initialized) {
        set_diag_status(&s_diag, OBD_PROVIDER_STATUS_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    if (ensure_provider_connected() != ESP_OK || !s_runtime.connected || !obd_is_connected()) {
        return ESP_OK;
    }

    if ((now - s_runtime.last_poll_tick) < pdMS_TO_TICKS(OBD_PROVIDER_POLL_INTERVAL_MS)) {
        return ESP_OK;
    }
    s_runtime.last_poll_tick = now;
    set_diag_status(&s_diag, OBD_PROVIDER_STATUS_POLLING);
    snprintf(s_diag.detail, sizeof(s_diag.detail), "Reading PID 0x%02X", pid);

    read_result = obd_read_pid(pid, &value);
    if (read_result < 0) {
        ESP_LOGW(TAG, "pid read failed pid=0x%02X rc=%d", pid, read_result);
        s_diag.failure_count++;
        set_diag_status(&s_diag, OBD_PROVIDER_STATUS_POLL_FAIL);
        snprintf(s_diag.detail, sizeof(s_diag.detail), "PID 0x%02X rc=%d", pid, read_result);
        if (!obd_is_connected()) {
            obd_end();
            mark_disconnected();
            *out_sample = s_runtime.sample;
            *out_has_update = true;
        }
        return ESP_OK;
    }

    s_runtime.next_pid_index = (s_runtime.next_pid_index + 1U) % (sizeof(s_pid_schedule) / sizeof(s_pid_schedule[0]));
    if (read_result == 0) {
        set_diag_status(&s_diag, OBD_PROVIDER_STATUS_POLL_WAIT);
        snprintf(s_diag.detail, sizeof(s_diag.detail), "PID 0x%02X no data", pid);
        return ESP_OK;
    }

    s_runtime.sample.connected = true;
    if (!update_sample_from_pid(pid, value, &s_runtime.sample)) {
        s_diag.failure_count++;
        set_diag_status(&s_diag, OBD_PROVIDER_STATUS_PARSE_FAIL);
        snprintf(s_diag.detail, sizeof(s_diag.detail), "Unhandled PID 0x%02X", pid);
        return ESP_OK;
    }

    s_diag.connected = true;
    s_diag.update_count++;
    set_diag_status(&s_diag, OBD_PROVIDER_STATUS_LIVE);
    snprintf(s_diag.detail, sizeof(s_diag.detail), "PID 0x%02X updated", pid);
    *out_sample = s_runtime.sample;
    *out_has_update = true;
    return ESP_OK;
}

static const char *backend_esp_obd_ii_name(void)
{
    return "esp_obd_ii";
}

static void backend_esp_obd_ii_get_diag(obd_provider_diagnostics_t *out_diag)
{
    if (out_diag == NULL) {
        return;
    }

    *out_diag = s_diag;
}

esp_err_t obd_provider_init(void)
{
    const obd_adapter_profile_t *active_profile = obd_adapter_profile_active_config();
    const obd_adapter_profile_t *vgate_profile = obd_adapter_profile_vgate_icar_pro_ble();

    ESP_LOGI(TAG,
             "obd adapter profile active=%s name_filter=%s svc=0x%04X notify=0x%04X write=0x%04X write_no_rsp=%d",
             obd_adapter_profile_detect_known_family(),
             active_profile->device_name_filter,
             active_profile->service_uuid16,
             active_profile->notify_uuid16,
             active_profile->write_uuid16,
             active_profile->write_without_response);
    ESP_LOGI(TAG,
             "vgate candidate profile name_filter=%s svc=0x%04X notify=0x%04X write=0x%04X write_no_rsp=%d",
             vgate_profile->device_name_filter,
             vgate_profile->service_uuid16,
             vgate_profile->notify_uuid16,
             vgate_profile->write_uuid16,
             vgate_profile->write_without_response);

    s_backend = service_ble_host_is_active() ? &k_service_ble_elm327_backend : &k_esp_obd_ii_backend;
    ESP_LOGI(TAG, "obd provider backend selected=%s", s_backend->name());
    return s_backend->init();
}

esp_err_t obd_provider_poll(obd_provider_sample_t *out_sample, bool *out_has_update)
{
    return s_backend->poll(out_sample, out_has_update);
}

const char *obd_provider_name(void)
{
    return s_backend->name();
}

void obd_provider_get_diagnostics(obd_provider_diagnostics_t *out_diag)
{
    if (out_diag == NULL) {
        return;
    }

    memset(out_diag, 0, sizeof(*out_diag));
    if (s_backend != NULL && s_backend->get_diag != NULL) {
        s_backend->get_diag(out_diag);
    }
}

const char *obd_provider_status_code_to_string(obd_provider_status_code_t status_code)
{
    switch (status_code) {
    case OBD_PROVIDER_STATUS_CONNECTING:
        return "connect";
    case OBD_PROVIDER_STATUS_CONNECT_FAIL:
        return "connect_fail";
    case OBD_PROVIDER_STATUS_DEVICE_NOT_FOUND:
        return "device_not_found";
    case OBD_PROVIDER_STATUS_GATT_MISMATCH:
        return "gatt_mismatch";
    case OBD_PROVIDER_STATUS_INITIALIZING:
        return "init";
    case OBD_PROVIDER_STATUS_INIT_FAIL:
        return "init_fail";
    case OBD_PROVIDER_STATUS_LIVE:
        return "live";
    case OBD_PROVIDER_STATUS_POLLING:
        return "poll";
    case OBD_PROVIDER_STATUS_POLL_WAIT:
        return "poll_wait";
    case OBD_PROVIDER_STATUS_POLL_FAIL:
        return "poll_fail";
    case OBD_PROVIDER_STATUS_RESPONSE_ERROR:
        return "response_error";
    case OBD_PROVIDER_STATUS_PARSE_FAIL:
        return "parse_fail";
    case OBD_PROVIDER_STATUS_WRITE_FAIL:
        return "write_fail";
    case OBD_PROVIDER_STATUS_INVALID_STATE:
        return "invalid_state";
    case OBD_PROVIDER_STATUS_IDLE:
    default:
        return "idle";
    }
}
