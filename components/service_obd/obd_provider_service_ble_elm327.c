#include "obd_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obd_adapter_profile.h"
#include "app_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service_ble_client.h"

static const char *TAG = "obd_ble_elm327";

enum {
    OBD_PROVIDER_CONNECT_RETRY_MS = 3000,
    OBD_PROVIDER_POLL_INTERVAL_MS = 250,
    OBD_PROVIDER_CONNECT_TIMEOUT_MS = 15000,
    OBD_PROVIDER_RX_TIMEOUT_MS = 1200,
    OBD_PROVIDER_INIT_TIMEOUT_MS = 1800,
    OBD_PROVIDER_RESPONSE_BUFFER_SIZE = 256,
};

typedef struct {
    bool initialized;
    bool connected;
    size_t next_pid_index;
    TickType_t last_connect_attempt_tick;
    TickType_t last_poll_tick;
    obd_provider_sample_t sample;
    service_ble_client_profile_t client_profile;
} obd_provider_runtime_t;

static obd_provider_runtime_t s_runtime;
static obd_provider_diagnostics_t s_diag;

typedef struct {
    const char *command;
    const char *expect;
    uint32_t timeout_ms;
} elm_init_command_t;

typedef struct {
    const char *command;
    const char *response_prefix;
    uint8_t pid;
} elm_pid_request_t;

static const elm_init_command_t s_init_sequence[] = {
    {.command = "ATZ", .expect = "ELM", .timeout_ms = OBD_PROVIDER_INIT_TIMEOUT_MS},
    {.command = "ATE0", .expect = "OK", .timeout_ms = OBD_PROVIDER_RX_TIMEOUT_MS},
    {.command = "ATL0", .expect = "OK", .timeout_ms = OBD_PROVIDER_RX_TIMEOUT_MS},
    {.command = "ATS0", .expect = "OK", .timeout_ms = OBD_PROVIDER_RX_TIMEOUT_MS},
    {.command = "ATH0", .expect = "OK", .timeout_ms = OBD_PROVIDER_RX_TIMEOUT_MS},
    {.command = "ATAT0", .expect = "OK", .timeout_ms = OBD_PROVIDER_RX_TIMEOUT_MS},
    {.command = "ATSP0", .expect = "OK", .timeout_ms = OBD_PROVIDER_RX_TIMEOUT_MS},
};

static const elm_pid_request_t s_pid_schedule[] = {
    {.command = "010C", .response_prefix = "410C", .pid = 0x0C},
    {.command = "010D", .response_prefix = "410D", .pid = 0x0D},
    {.command = "0105", .response_prefix = "4105", .pid = 0x05},
    {.command = "0111", .response_prefix = "4111", .pid = 0x11},
    {.command = "012F", .response_prefix = "412F", .pid = 0x2F},
};

static void set_diag_text(char *buffer, size_t buffer_size, const char *text);
static void set_diag_compose(char *buffer,
                             size_t buffer_size,
                             const char *prefix,
                             const char *separator,
                             const char *suffix);
static void copy_ble_client_timings(const service_ble_client_diagnostics_t *client_diag);
static void copy_ble_client_failure(const service_ble_client_diagnostics_t *client_diag);
static void update_status_from_client_diag(const service_ble_client_diagnostics_t *client_diag, esp_err_t open_err);

static void set_diag_status(obd_provider_status_code_t status_code)
{
    s_diag.status_code = (uint8_t)status_code;
    set_diag_text(s_diag.stage, sizeof(s_diag.stage), obd_provider_status_code_to_string(status_code));
}

static void copy_ble_client_timings(const service_ble_client_diagnostics_t *client_diag)
{
    if (client_diag == NULL) {
        s_diag.ble_scan_elapsed_ms = 0;
        s_diag.ble_connect_elapsed_ms = 0;
        s_diag.ble_discovery_elapsed_ms = 0;
        return;
    }

    s_diag.ble_scan_elapsed_ms = client_diag->scan_elapsed_ms;
    s_diag.ble_connect_elapsed_ms = client_diag->connect_elapsed_ms;
    s_diag.ble_discovery_elapsed_ms = client_diag->discovery_elapsed_ms;
}

static void copy_ble_client_failure(const service_ble_client_diagnostics_t *client_diag)
{
    if (client_diag == NULL) {
        s_diag.ble_last_failure_rc = 0;
        s_diag.ble_last_failure_stage[0] = '\0';
        return;
    }

    s_diag.ble_last_failure_rc = client_diag->last_failure_rc;
    set_diag_text(s_diag.ble_last_failure_stage,
                  sizeof(s_diag.ble_last_failure_stage),
                  service_ble_client_status_code_to_string(
                      (service_ble_client_status_code_t)client_diag->last_failure_status_code));
}

static void update_status_from_client_diag(const service_ble_client_diagnostics_t *client_diag, esp_err_t open_err)
{
    if (client_diag == NULL) {
        copy_ble_client_timings(NULL);
        copy_ble_client_failure(NULL);
        set_diag_status(open_err == ESP_ERR_TIMEOUT ? OBD_PROVIDER_STATUS_DEVICE_NOT_FOUND
                                                    : OBD_PROVIDER_STATUS_CONNECT_FAIL);
        set_diag_compose(s_diag.detail, sizeof(s_diag.detail), "BLE open", " ", esp_err_to_name(open_err));
        return;
    }

    copy_ble_client_timings(client_diag);
    copy_ble_client_failure(client_diag);
    switch ((service_ble_client_status_code_t)client_diag->status_code) {
    case SERVICE_BLE_CLIENT_STATUS_SCAN_TIMEOUT:
        set_diag_status(OBD_PROVIDER_STATUS_DEVICE_NOT_FOUND);
        break;
    case SERVICE_BLE_CLIENT_STATUS_SERVICE_NOT_FOUND:
    case SERVICE_BLE_CLIENT_STATUS_CHARACTERISTIC_NOT_FOUND:
    case SERVICE_BLE_CLIENT_STATUS_DESCRIPTOR_NOT_FOUND:
    case SERVICE_BLE_CLIENT_STATUS_DISCOVERY_FAIL:
        set_diag_status(OBD_PROVIDER_STATUS_GATT_MISMATCH);
        break;
    case SERVICE_BLE_CLIENT_STATUS_HOST_NOT_READY:
    case SERVICE_BLE_CLIENT_STATUS_INVALID_PROFILE:
    case SERVICE_BLE_CLIENT_STATUS_NO_MEMORY:
        set_diag_status(OBD_PROVIDER_STATUS_INVALID_STATE);
        break;
    case SERVICE_BLE_CLIENT_STATUS_SUBSCRIBE_FAIL:
    case SERVICE_BLE_CLIENT_STATUS_CONNECT_FAIL:
    case SERVICE_BLE_CLIENT_STATUS_SCAN_START_FAIL:
    case SERVICE_BLE_CLIENT_STATUS_DISCONNECTED:
        set_diag_status(OBD_PROVIDER_STATUS_CONNECT_FAIL);
        break;
    case SERVICE_BLE_CLIENT_STATUS_CONNECTING:
    case SERVICE_BLE_CLIENT_STATUS_DEVICE_FOUND:
    case SERVICE_BLE_CLIENT_STATUS_SCANNING:
        set_diag_status(OBD_PROVIDER_STATUS_CONNECTING);
        break;
    case SERVICE_BLE_CLIENT_STATUS_CONNECTED:
        set_diag_status(OBD_PROVIDER_STATUS_INITIALIZING);
        break;
    case SERVICE_BLE_CLIENT_STATUS_IDLE:
    default:
        set_diag_status(open_err == ESP_ERR_TIMEOUT ? OBD_PROVIDER_STATUS_DEVICE_NOT_FOUND
                                                    : OBD_PROVIDER_STATUS_CONNECT_FAIL);
        break;
    }

    if (client_diag->detail[0] != '\0') {
        set_diag_compose(s_diag.detail,
                         sizeof(s_diag.detail),
                         service_ble_client_status_code_to_string(
                             (service_ble_client_status_code_t)client_diag->status_code),
                         ": ",
                         client_diag->detail);
    } else {
        set_diag_compose(s_diag.detail, sizeof(s_diag.detail), "BLE open", " ", esp_err_to_name(open_err));
    }
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

static void set_diag_compose(char *buffer,
                             size_t buffer_size,
                             const char *prefix,
                             const char *separator,
                             const char *suffix)
{
    size_t length = 0;

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (prefix != NULL && prefix[0] != '\0') {
        set_diag_text(buffer, buffer_size, prefix);
        length = strnlen(buffer, buffer_size);
    }

    if (separator != NULL && separator[0] != '\0' && suffix != NULL && suffix[0] != '\0' && length < (buffer_size - 1U)) {
        snprintf(buffer + length, buffer_size - length, "%s", separator);
        length = strnlen(buffer, buffer_size);
    }

    if (suffix != NULL && suffix[0] != '\0' && length < (buffer_size - 1U)) {
        snprintf(buffer + length, buffer_size - length, "%s", suffix);
    }
}

static void mark_disconnected(void)
{
    s_runtime.connected = false;
    s_runtime.sample.connected = false;
    s_diag.connected = false;
    service_ble_client_close();
}

static void reset_rx_stream(void)
{
    uint8_t dump[64] = {0};
    while (service_ble_client_read(dump, sizeof(dump), 0) > 0) {
    }
}

static esp_err_t send_ascii_command(const char *command, char *response, size_t response_size, uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    size_t response_len = 0;
    char command_buffer[24] = {0};

    if (command == NULL || response == NULL || response_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    reset_rx_stream();
    snprintf(command_buffer, sizeof(command_buffer), "%s\r", command);
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), command);
    if (service_ble_client_write((const uint8_t *)command_buffer, strlen(command_buffer)) != ESP_OK) {
        s_diag.failure_count++;
        set_diag_status(OBD_PROVIDER_STATUS_WRITE_FAIL);
        return ESP_FAIL;
    }

    response[0] = '\0';
    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t chunk[64] = {0};
        const int bytes_read = service_ble_client_read(chunk, sizeof(chunk), 120);
        if (bytes_read < 0) {
            return ESP_FAIL;
        }
        if (bytes_read == 0) {
            continue;
        }

        for (int i = 0; i < bytes_read; ++i) {
            if (response_len + 1U < response_size) {
                response[response_len++] = (char)chunk[i];
                response[response_len] = '\0';
            }
            if (chunk[i] == '>') {
                return ESP_OK;
            }
        }
    }

    return ESP_ERR_TIMEOUT;
}

static void normalize_response(const char *input, char *output, size_t output_size)
{
    size_t out_len = 0;

    if (input == NULL || output == NULL || output_size == 0U) {
        return;
    }

    for (size_t i = 0; input[i] != '\0' && out_len + 1U < output_size; ++i) {
        unsigned char ch = (unsigned char)input[i];
        if (ch == '>' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t' || ch == ':') {
            continue;
        }
        output[out_len++] = (char)toupper(ch);
    }

    output[out_len] = '\0';
}

static bool response_has_failure(const char *normalized)
{
    return normalized == NULL ||
           strstr(normalized, "NODATA") != NULL ||
           strstr(normalized, "ERROR") != NULL ||
           strstr(normalized, "?") != NULL ||
           strstr(normalized, "UNABLETOCONNECT") != NULL;
}

static bool parse_hex_byte(const char *ptr, uint8_t *out_value)
{
    char temp[3] = {0};

    if (ptr == NULL || out_value == NULL || !isxdigit((unsigned char)ptr[0]) || !isxdigit((unsigned char)ptr[1])) {
        return false;
    }

    temp[0] = ptr[0];
    temp[1] = ptr[1];
    *out_value = (uint8_t)strtoul(temp, NULL, 16);
    return true;
}

static bool update_sample_from_pid_response(const elm_pid_request_t *request,
                                            const char *normalized,
                                            obd_provider_sample_t *sample)
{
    const char *match = NULL;
    uint8_t byte_a = 0;
    uint8_t byte_b = 0;

    if (request == NULL || normalized == NULL || sample == NULL) {
        return false;
    }

    match = strstr(normalized, request->response_prefix);
    if (match == NULL) {
        return false;
    }
    match += strlen(request->response_prefix);

    switch (request->pid) {
    case 0x0C:
        if (!parse_hex_byte(match, &byte_a) || !parse_hex_byte(match + 2, &byte_b)) {
            return false;
        }
        sample->rpm = (uint16_t)((((uint16_t)byte_a << 8U) | byte_b) / 4U);
        return true;
    case 0x0D:
        if (!parse_hex_byte(match, &byte_a)) {
            return false;
        }
        sample->speed_kmh = byte_a;
        return true;
    case 0x05:
        if (!parse_hex_byte(match, &byte_a)) {
            return false;
        }
        sample->coolant_temp_c = (int8_t)((int)byte_a - 40);
        return true;
    case 0x11:
        if (!parse_hex_byte(match, &byte_a)) {
            return false;
        }
        sample->throttle_percent = (uint8_t)((byte_a * 100U) / 255U);
        return true;
    case 0x2F:
        if (!parse_hex_byte(match, &byte_a)) {
            return false;
        }
        sample->fuel_percent = (uint8_t)((byte_a * 100U) / 255U);
        return true;
    default:
        return false;
    }
}

static esp_err_t run_adapter_init_sequence(void)
{
    char raw_response[OBD_PROVIDER_RESPONSE_BUFFER_SIZE] = {0};
    char normalized[OBD_PROVIDER_RESPONSE_BUFFER_SIZE] = {0};

    for (size_t i = 0; i < (sizeof(s_init_sequence) / sizeof(s_init_sequence[0])); ++i) {
        const elm_init_command_t *step = &s_init_sequence[i];
        set_diag_status(OBD_PROVIDER_STATUS_INITIALIZING);
        snprintf(s_diag.detail, sizeof(s_diag.detail), "%s", step->command);
        const esp_err_t command_err = send_ascii_command(step->command,
                                                         raw_response,
                                                         sizeof(raw_response),
                                                         step->timeout_ms);
        if (command_err != ESP_OK) {
            s_diag.failure_count++;
            set_diag_status(OBD_PROVIDER_STATUS_INIT_FAIL);
            APP_LOGW(TAG, APP_ERR_OBD_INIT, "elm init cmd=%s err=%s", step->command, esp_err_to_name(command_err));
            return command_err;
        }

        normalize_response(raw_response, normalized, sizeof(normalized));
        if (step->expect != NULL && strstr(normalized, step->expect) == NULL) {
            s_diag.failure_count++;
            set_diag_status(OBD_PROVIDER_STATUS_INIT_FAIL);
            set_diag_compose(s_diag.detail, sizeof(s_diag.detail), step->command, " -> ", normalized);
            APP_LOGW(TAG,
                     APP_ERR_OBD_INIT,
                     "elm init cmd=%s unexpected response=%s",
                     step->command,
                     normalized);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    set_diag_status(OBD_PROVIDER_STATUS_INITIALIZING);
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), "ELM327 adapter initialized");
    return ESP_OK;
}

static esp_err_t ensure_provider_connected(void)
{
    const TickType_t now = xTaskGetTickCount();
    const obd_adapter_profile_t *active_profile = obd_adapter_profile_active_config();

    if (s_runtime.connected && service_ble_client_is_connected()) {
        return ESP_OK;
    }

    if ((now - s_runtime.last_connect_attempt_tick) < pdMS_TO_TICKS(OBD_PROVIDER_CONNECT_RETRY_MS)) {
        return ESP_OK;
    }

    s_runtime.last_connect_attempt_tick = now;
    s_diag.connect_attempt_count++;
    set_diag_status(OBD_PROVIDER_STATUS_CONNECTING);
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), active_profile->device_name_filter);
    s_runtime.client_profile.device_name_filter = active_profile->device_name_filter;
    s_runtime.client_profile.service_uuid16 = active_profile->service_uuid16;
    s_runtime.client_profile.notify_uuid16 = active_profile->notify_uuid16;
    s_runtime.client_profile.write_uuid16 = active_profile->write_uuid16;
    s_runtime.client_profile.write_without_response = active_profile->write_without_response;

    const esp_err_t open_err = service_ble_client_open(&s_runtime.client_profile, OBD_PROVIDER_CONNECT_TIMEOUT_MS);
    if (open_err != ESP_OK) {
        service_ble_client_diagnostics_t client_diag = {0};
        service_ble_client_get_diagnostics(&client_diag);
        mark_disconnected();
        s_diag.failure_count++;
        update_status_from_client_diag(&client_diag, open_err);
        APP_LOGW(TAG, APP_ERR_OBD_INIT, "open elm327 ble client failed: %s", esp_err_to_name(open_err));
        return ESP_OK;
    }

    service_ble_client_diagnostics_t client_diag = {0};
    service_ble_client_get_diagnostics(&client_diag);
    copy_ble_client_timings(&client_diag);
    copy_ble_client_failure(&client_diag);
    set_diag_status(OBD_PROVIDER_STATUS_INITIALIZING);
    if (client_diag.detail[0] != '\0') {
        set_diag_compose(s_diag.detail, sizeof(s_diag.detail), "ble", " ", client_diag.detail);
    } else {
        set_diag_text(s_diag.detail, sizeof(s_diag.detail), "BLE link ready");
    }
    const esp_err_t init_err = run_adapter_init_sequence();
    if (init_err != ESP_OK) {
        mark_disconnected();
        return ESP_OK;
    }

    s_runtime.connected = true;
    s_runtime.sample.connected = true;
    s_diag.connected = true;
    set_diag_status(OBD_PROVIDER_STATUS_LIVE);
    service_ble_client_get_diagnostics(&client_diag);
    copy_ble_client_timings(&client_diag);
    copy_ble_client_failure(&client_diag);
    if (client_diag.detail[0] != '\0') {
        set_diag_text(s_diag.detail, sizeof(s_diag.detail), client_diag.detail);
    } else {
        char service_uuid[12] = {0};
        snprintf(service_uuid, sizeof(service_uuid), "0x%04X", s_runtime.client_profile.service_uuid16);
        set_diag_compose(s_diag.detail,
                         sizeof(s_diag.detail),
                         s_runtime.client_profile.device_name_filter,
                         " ",
                         service_uuid);
    }
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "service_ble ELM327 connected name_filter=%s svc=0x%04X notify=0x%04X write=0x%04X",
             s_runtime.client_profile.device_name_filter,
             s_runtime.client_profile.service_uuid16,
             s_runtime.client_profile.notify_uuid16,
             s_runtime.client_profile.write_uuid16);
    return ESP_OK;
}

esp_err_t obd_provider_service_ble_elm327_init(void)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    memset(&s_diag, 0, sizeof(s_diag));
    s_runtime.initialized = true;
    set_diag_text(s_diag.backend_name, sizeof(s_diag.backend_name), "service_ble_elm327");
    set_diag_status(OBD_PROVIDER_STATUS_IDLE);
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), "Waiting for BLE adapter");
    return ESP_OK;
}

esp_err_t obd_provider_service_ble_elm327_poll(obd_provider_sample_t *out_sample, bool *out_has_update)
{
    char raw_response[OBD_PROVIDER_RESPONSE_BUFFER_SIZE] = {0};
    char normalized[OBD_PROVIDER_RESPONSE_BUFFER_SIZE] = {0};
    const TickType_t now = xTaskGetTickCount();
    const elm_pid_request_t *request = &s_pid_schedule[s_runtime.next_pid_index];

    if (out_sample == NULL || out_has_update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_has_update = false;

    if (!s_runtime.initialized) {
        set_diag_status(OBD_PROVIDER_STATUS_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    if (ensure_provider_connected() != ESP_OK || !s_runtime.connected || !service_ble_client_is_connected()) {
        return ESP_OK;
    }

    if ((now - s_runtime.last_poll_tick) < pdMS_TO_TICKS(OBD_PROVIDER_POLL_INTERVAL_MS)) {
        return ESP_OK;
    }
    s_runtime.last_poll_tick = now;
    set_diag_status(OBD_PROVIDER_STATUS_POLLING);
    snprintf(s_diag.detail, sizeof(s_diag.detail), "%s", request->command);

    const esp_err_t command_err = send_ascii_command(request->command,
                                                     raw_response,
                                                     sizeof(raw_response),
                                                     OBD_PROVIDER_RX_TIMEOUT_MS);
    if (command_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_OBD_INIT, "elm pid cmd=%s err=%s", request->command, esp_err_to_name(command_err));
        s_diag.failure_count++;
        set_diag_status(OBD_PROVIDER_STATUS_POLL_FAIL);
        set_diag_compose(s_diag.detail, sizeof(s_diag.detail), request->command, " ", esp_err_to_name(command_err));
        mark_disconnected();
        *out_sample = s_runtime.sample;
        *out_has_update = true;
        return ESP_OK;
    }

    normalize_response(raw_response, normalized, sizeof(normalized));
    s_runtime.next_pid_index = (s_runtime.next_pid_index + 1U) % (sizeof(s_pid_schedule) / sizeof(s_pid_schedule[0]));

    if (response_has_failure(normalized)) {
        s_diag.failure_count++;
        set_diag_status(OBD_PROVIDER_STATUS_RESPONSE_ERROR);
        set_diag_compose(s_diag.detail, sizeof(s_diag.detail), request->command, " -> ", normalized);
        APP_LOGW(TAG, APP_ERR_OBD_PARSE, "elm pid cmd=%s response=%s", request->command, normalized);
        return ESP_OK;
    }

    s_runtime.sample.connected = true;
    if (!update_sample_from_pid_response(request, normalized, &s_runtime.sample)) {
        s_diag.failure_count++;
        set_diag_status(OBD_PROVIDER_STATUS_PARSE_FAIL);
        set_diag_compose(s_diag.detail, sizeof(s_diag.detail), request->command, " -> ", normalized);
        APP_LOGW(TAG, APP_ERR_OBD_PARSE, "elm pid parse miss cmd=%s response=%s", request->command, normalized);
        return ESP_OK;
    }

    s_diag.connected = true;
    s_diag.update_count++;
    set_diag_status(OBD_PROVIDER_STATUS_LIVE);
    snprintf(s_diag.detail, sizeof(s_diag.detail), "%s ok", request->command);
    *out_sample = s_runtime.sample;
    *out_has_update = true;
    return ESP_OK;
}

const char *obd_provider_service_ble_elm327_name(void)
{
    return "service_ble_elm327";
}

void obd_provider_service_ble_elm327_get_diag(obd_provider_diagnostics_t *out_diag)
{
    if (out_diag == NULL) {
        return;
    }

    *out_diag = s_diag;
}
