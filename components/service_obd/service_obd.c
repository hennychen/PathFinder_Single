#include "service_obd.h"

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "app_status.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "obd_provider.h"

static const char *TAG = "service_obd";

enum {
    OBD_TASK_STACK = 4096,
    OBD_TASK_PRIORITY = 2,
    OBD_TASK_CORE = 0,
    OBD_QUEUE_LEN = 8,
    OBD_TASK_WAIT_MS = 200,
    OBD_PROVIDER_GRACE_MS = SERVICE_OBD_TIMEOUT_MS,
    OBD_ALERT_RPM_THRESHOLD = 4500,
    OBD_ALERT_SPEED_THRESHOLD = 120,
    OBD_ALERT_COOLANT_THRESHOLD = 105,
};

typedef struct {
    size_t packet_len;
    uint8_t packet[OBD_PACKET_SIZE];
} obd_command_t;

static bool s_ready;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_task_handle;
static obd_runtime_state_t s_obd_state;
static TickType_t s_last_sample_tick;
static TickType_t s_last_bridge_tick;
static bool s_provider_enabled;

static void build_alert_text(const obd_packet_t *packet, char *buffer, size_t buffer_size, bool *out_alert_active);

static uint32_t tick_to_ms(TickType_t tick)
{
    return (uint32_t)tick * portTICK_PERIOD_MS;
}

static void refresh_provider_diagnostics(void)
{
    obd_provider_diagnostics_t diag = {0};

    obd_provider_get_diagnostics(&diag);
    s_obd_state.provider_status_code = diag.status_code;
    s_obd_state.provider_connect_attempt_count = diag.connect_attempt_count;
    s_obd_state.provider_failure_count = diag.failure_count;
    s_obd_state.provider_update_count = diag.update_count;
    s_obd_state.provider_ble_scan_elapsed_ms = diag.ble_scan_elapsed_ms;
    s_obd_state.provider_ble_connect_elapsed_ms = diag.ble_connect_elapsed_ms;
    s_obd_state.provider_ble_discovery_elapsed_ms = diag.ble_discovery_elapsed_ms;
    s_obd_state.provider_ble_last_failure_rc = diag.ble_last_failure_rc;
    s_obd_state.connected = s_obd_state.connected || diag.connected;

    strncpy(s_obd_state.provider_name, diag.backend_name, sizeof(s_obd_state.provider_name) - 1U);
    s_obd_state.provider_name[sizeof(s_obd_state.provider_name) - 1U] = '\0';
    if (diag.stage[0] != '\0') {
        strncpy(s_obd_state.provider_stage, diag.stage, sizeof(s_obd_state.provider_stage) - 1U);
        s_obd_state.provider_stage[sizeof(s_obd_state.provider_stage) - 1U] = '\0';
    } else {
        strncpy(s_obd_state.provider_stage,
                obd_provider_status_code_to_string((obd_provider_status_code_t)diag.status_code),
                sizeof(s_obd_state.provider_stage) - 1U);
        s_obd_state.provider_stage[sizeof(s_obd_state.provider_stage) - 1U] = '\0';
    }
    strncpy(s_obd_state.provider_ble_last_failure_stage,
            diag.ble_last_failure_stage,
            sizeof(s_obd_state.provider_ble_last_failure_stage) - 1U);
    s_obd_state.provider_ble_last_failure_stage[sizeof(s_obd_state.provider_ble_last_failure_stage) - 1U] = '\0';
    strncpy(s_obd_state.provider_detail, diag.detail, sizeof(s_obd_state.provider_detail) - 1U);
    s_obd_state.provider_detail[sizeof(s_obd_state.provider_detail) - 1U] = '\0';
}

static void publish_obd_state(void)
{
    refresh_provider_diagnostics();
    app_state_set_obd_state(&s_obd_state);
}

static void apply_obd_packet(const obd_packet_t *packet, TickType_t now)
{
    if (packet == NULL) {
        return;
    }

    s_last_sample_tick = now;
    s_obd_state.connected = packet->connected;
    s_obd_state.speed_kmh = packet->speed_kmh;
    s_obd_state.rpm = packet->rpm;
    s_obd_state.coolant_temp_c = packet->coolant_temp_c;
    s_obd_state.throttle_percent = packet->throttle_percent;
    s_obd_state.fuel_percent = packet->fuel_percent;
    s_obd_state.sample_count++;
    s_obd_state.last_update_ms = tick_to_ms(now);
    build_alert_text(packet,
                     s_obd_state.alert_text,
                     sizeof(s_obd_state.alert_text),
                     &s_obd_state.alert_active);

    publish_obd_state();
    APP_LOGI(TAG,
             APP_EVT_OBD_SAMPLE,
             "obd sample speed=%u rpm=%u coolant=%d throttle=%u fuel=%u connected=%d provider=%s stage=%s samples=%lu",
             packet->speed_kmh,
             packet->rpm,
             packet->coolant_temp_c,
             packet->throttle_percent,
             packet->fuel_percent,
             packet->connected,
             s_obd_state.provider_name,
             s_obd_state.provider_stage,
             (unsigned long)s_obd_state.sample_count);

    if (s_obd_state.alert_active) {
        APP_LOGW(TAG, APP_EVT_OBD_ALERT, "%s", s_obd_state.alert_text);
    }
}

static bool is_bridge_source_active(TickType_t now)
{
    return s_last_bridge_tick != 0 &&
           (now - s_last_bridge_tick) < pdMS_TO_TICKS(OBD_PROVIDER_GRACE_MS);
}

static void build_alert_text(const obd_packet_t *packet, char *buffer, size_t buffer_size, bool *out_alert_active)
{
    bool alert_active = false;

    if (packet->coolant_temp_c >= OBD_ALERT_COOLANT_THRESHOLD) {
        snprintf(buffer, buffer_size, "Coolant high: %d C", packet->coolant_temp_c);
        alert_active = true;
    } else if (packet->rpm >= OBD_ALERT_RPM_THRESHOLD) {
        snprintf(buffer, buffer_size, "RPM high: %u", packet->rpm);
        alert_active = true;
    } else if (packet->speed_kmh >= OBD_ALERT_SPEED_THRESHOLD) {
        snprintf(buffer, buffer_size, "Speed high: %u km/h", packet->speed_kmh);
        alert_active = true;
    } else if (packet->connected) {
        snprintf(buffer, buffer_size, "Live telemetry healthy");
    } else {
        snprintf(buffer, buffer_size, "Waiting for ELM327 session");
    }

    if (out_alert_active != NULL) {
        *out_alert_active = alert_active;
    }
}

static void handle_obd_packet(const obd_command_t *command)
{
    obd_packet_t packet = {0};
    const esp_err_t parse_err = obd_protocol_parse_packet(command->packet, command->packet_len, &packet);
    if (parse_err != ESP_OK) {
        APP_LOGW(TAG,
                 APP_ERR_OBD_PARSE,
                 "drop obd packet len=%u err=%s",
                 (unsigned int)command->packet_len,
                 esp_err_to_name(parse_err));
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    s_last_bridge_tick = now;
    apply_obd_packet(&packet, now);
}

static void process_obd_provider(void)
{
    if (!s_provider_enabled) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    obd_provider_sample_t sample = {0};
    obd_packet_t packet = {0};
    bool has_update = false;
    const esp_err_t poll_err = obd_provider_poll(&sample, &has_update);
    if (poll_err != ESP_OK || !has_update || is_bridge_source_active(now)) {
        return;
    }

    packet.connected = sample.connected;
    packet.speed_kmh = sample.speed_kmh;
    packet.rpm = sample.rpm;
    packet.coolant_temp_c = sample.coolant_temp_c;
    packet.throttle_percent = sample.throttle_percent;
    packet.fuel_percent = sample.fuel_percent;
    apply_obd_packet(&packet, now);
}

static void process_obd_timeout(void)
{
    if (!s_obd_state.connected) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if ((now - s_last_sample_tick) < pdMS_TO_TICKS(SERVICE_OBD_TIMEOUT_MS)) {
        return;
    }

    s_obd_state.connected = false;
    s_obd_state.alert_active = false;
    snprintf(s_obd_state.alert_text, sizeof(s_obd_state.alert_text), "ELM327 telemetry timed out");
    publish_obd_state();
    APP_LOGW(TAG, APP_ERR_OBD_INIT, "obd telemetry timed out after %dms", SERVICE_OBD_TIMEOUT_MS);
}

static void task_obd_runtime(void *arg)
{
    (void)arg;
    s_ready = true;
    s_last_bridge_tick = 0;
    snprintf(s_obd_state.alert_text, sizeof(s_obd_state.alert_text), "Waiting for ELM327 session");
    publish_obd_state();

    while (true) {
        obd_command_t command = {0};
        if (xQueueReceive(s_command_queue, &command, pdMS_TO_TICKS(OBD_TASK_WAIT_MS)) == pdTRUE) {
            handle_obd_packet(&command);
        }

        process_obd_provider();
        process_obd_timeout();
    }
}

esp_err_t service_obd_init(void)
{
    if (s_command_queue != NULL && s_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(obd_provider_init(), TAG, "obd provider init failed");
    s_provider_enabled = true;

    s_command_queue = xQueueCreate(OBD_QUEUE_LEN, sizeof(obd_command_t));
    if (s_command_queue == NULL) {
        APP_LOGE(TAG, APP_ERR_OBD_INIT, "create obd queue failed");
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(task_obd_runtime,
                                                       "task_obd",
                                                       OBD_TASK_STACK,
                                                       NULL,
                                                       OBD_TASK_PRIORITY,
                                                       &s_task_handle,
                                                       OBD_TASK_CORE);
    if (created != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        s_task_handle = NULL;
        APP_LOGE(TAG, APP_ERR_OBD_INIT, "create task_obd failed");
        return ESP_ERR_NO_MEM;
    }

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "obd service ready provider=%s core=%d priority=%d timeout=%dms",
             obd_provider_name(),
             OBD_TASK_CORE,
             OBD_TASK_PRIORITY,
             SERVICE_OBD_TIMEOUT_MS);
    return ESP_OK;
}

bool service_obd_is_ready(void)
{
    return s_ready;
}

esp_err_t service_obd_submit_packet(const uint8_t *packet, size_t packet_len)
{
    obd_command_t command = {0};

    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len != OBD_PACKET_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    command.packet_len = packet_len;
    memcpy(command.packet, packet, packet_len);
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        APP_LOGW(TAG, APP_ERR_OBD_INIT, "obd queue full len=%u", (unsigned int)packet_len);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t service_obd_submit_mock_sample(uint16_t speed_kmh,
                                         uint16_t rpm,
                                         int8_t coolant_temp_c,
                                         uint8_t throttle_percent,
                                         uint8_t fuel_percent,
                                         bool connected)
{
    const obd_packet_t packet = {
        .connected = connected,
        .speed_kmh = speed_kmh,
        .rpm = rpm,
        .coolant_temp_c = coolant_temp_c,
        .throttle_percent = throttle_percent,
        .fuel_percent = fuel_percent,
    };
    uint8_t encoded[OBD_PACKET_SIZE] = {0};
    size_t encoded_len = 0;
    const esp_err_t encode_err = obd_protocol_encode_packet(&packet, encoded, sizeof(encoded), &encoded_len);
    if (encode_err != ESP_OK) {
        return encode_err;
    }

    return service_obd_submit_packet(encoded, encoded_len);
}
