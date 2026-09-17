#include "service_navigation.h"

#include <string.h>

#include "app_pages.h"
#include "app_state.h"
#include "app_status.h"
#include "service_ble.h"
#include "service_obd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "service_navigation";

enum {
    NAVIGATION_TASK_STACK = 4096,
    NAVIGATION_TASK_PRIORITY = 3,
    NAVIGATION_TASK_CORE = 0,
    NAVIGATION_PACKET_QUEUE_LEN = 8,
    NAVIGATION_TASK_WAIT_MS = 100,
    NAVIGATION_FRAGMENT_TIMEOUT_MS = 1000,
};

typedef struct {
    size_t packet_len;
    uint8_t packet[NAVIGATION_PACKET_MAX_SIZE];
} navigation_command_t;

typedef struct {
    bool active;
    uint8_t message_id;
    uint8_t chunk_count;
    uint8_t next_chunk_index;
    size_t packet_len;
    TickType_t last_update_tick;
    uint8_t packet[NAVIGATION_PACKET_MAX_SIZE];
} navigation_fragment_state_t;

static bool s_ready;
static QueueHandle_t s_packet_queue;
static TaskHandle_t s_task_handle;
static navigation_runtime_state_t s_nav_state;
static TickType_t s_last_packet_tick;
static navigation_fragment_state_t s_fragment_state;

static uint32_t tick_to_ms(TickType_t tick)
{
    return (uint32_t)tick * portTICK_PERIOD_MS;
}

static void publish_navigation_state(void)
{
    app_state_set_navigation_state(&s_nav_state);
}

static bool is_fragment_frame(const uint8_t *packet, size_t packet_len)
{
    return packet != NULL && packet_len >= NAVIGATION_FRAGMENT_HEADER_SIZE && packet[0] == NAVIGATION_FRAGMENT_MARKER;
}

static void reset_fragment_state(void)
{
    memset(&s_fragment_state, 0, sizeof(s_fragment_state));
}

static esp_err_t enqueue_navigation_packet(const uint8_t *packet, size_t packet_len)
{
    navigation_command_t command = {0};

    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len < NAVIGATION_PACKET_MIN_SIZE || packet_len > NAVIGATION_PACKET_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_packet_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    command.packet_len = packet_len;
    memcpy(command.packet, packet, packet_len);
    if (xQueueSend(s_packet_queue, &command, 0) != pdTRUE) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "navigation packet queue full len=%u", (unsigned int)packet_len);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t submit_fragment_frame(const uint8_t *packet, size_t packet_len)
{
    const uint8_t message_id = packet[1];
    const uint8_t chunk_index = packet[2];
    const uint8_t chunk_count = packet[3];
    const uint8_t chunk_len = packet[4];
    const TickType_t now = xTaskGetTickCount();

    if (chunk_count == 0U || chunk_index >= chunk_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len != (size_t)NAVIGATION_FRAGMENT_HEADER_SIZE + chunk_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (chunk_len == 0U || chunk_len > NAVIGATION_FRAGMENT_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (chunk_index == 0U) {
        reset_fragment_state();
        s_fragment_state.active = true;
        s_fragment_state.message_id = message_id;
        s_fragment_state.chunk_count = chunk_count;
        s_fragment_state.next_chunk_index = 0;
    }

    if (!s_fragment_state.active ||
        s_fragment_state.message_id != message_id ||
        s_fragment_state.chunk_count != chunk_count ||
        s_fragment_state.next_chunk_index != chunk_index) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((s_fragment_state.packet_len + chunk_len) > NAVIGATION_PACKET_MAX_SIZE) {
        reset_fragment_state();
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&s_fragment_state.packet[s_fragment_state.packet_len], &packet[NAVIGATION_FRAGMENT_HEADER_SIZE], chunk_len);
    s_fragment_state.packet_len += chunk_len;
    s_fragment_state.next_chunk_index++;
    s_fragment_state.last_update_tick = now;

    APP_LOGI(TAG,
             APP_EVT_NAV_PACKET,
             "nav fragment msg=%u chunk=%u/%u len=%u total=%u",
             message_id,
             (unsigned int)(chunk_index + 1U),
             chunk_count,
             chunk_len,
             (unsigned int)s_fragment_state.packet_len);

    if (s_fragment_state.next_chunk_index < s_fragment_state.chunk_count) {
        return ESP_OK;
    }

    const esp_err_t submit_err = enqueue_navigation_packet(s_fragment_state.packet, s_fragment_state.packet_len);
    if (submit_err == ESP_OK) {
        APP_LOGI(TAG,
                 APP_EVT_NAV_PACKET,
                 "nav fragment msg=%u reassembled len=%u",
                 message_id,
                 (unsigned int)s_fragment_state.packet_len);
    }
    reset_fragment_state();
    return submit_err;
}

static void handle_navigation_parse_error(esp_err_t err, size_t packet_len)
{
    s_nav_state.invalid_packet_count++;
    publish_navigation_state();
    APP_LOGW(TAG,
             APP_ERR_NAV_PARSE,
             "drop nav packet len=%u err=%s invalid=%lu",
             (unsigned int)packet_len,
             esp_err_to_name(err),
             (unsigned long)s_nav_state.invalid_packet_count);
}

static void handle_navigation_packet(const navigation_command_t *command)
{
    navigation_packet_t packet = {0};
    const esp_err_t parse_err = navigation_protocol_parse_packet(command->packet, command->packet_len, &packet);
    if (parse_err != ESP_OK) {
        handle_navigation_parse_error(parse_err, command->packet_len);
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    s_last_packet_tick = now;
    s_nav_state.active = true;
    s_nav_state.turn_type = packet.turn_type;
    s_nav_state.step_distance_m = packet.step_distance_m;
    s_nav_state.total_distance_m = packet.total_distance_m;
    s_nav_state.remain_time_s = packet.remain_time_s;
    s_nav_state.packet_count++;
    s_nav_state.last_update_ms = tick_to_ms(now);
    memcpy(s_nav_state.road_name, packet.road_name, sizeof(s_nav_state.road_name));

    publish_navigation_state();
    APP_LOGI(TAG,
             APP_EVT_NAV_PACKET,
             "nav packet turn=%s step=%um total=%um remain=%us road=%s packets=%lu",
             navigation_protocol_turn_to_string(packet.turn_type),
             packet.step_distance_m,
             packet.total_distance_m,
             packet.remain_time_s,
             packet.road_name_len > 0U ? packet.road_name : "<empty>",
             (unsigned long)s_nav_state.packet_count);
}

static void process_navigation_timeout(void)
{
    const TickType_t now = xTaskGetTickCount();

    if (!s_nav_state.active) {
        goto fragment_timeout;
    }

    if ((now - s_last_packet_tick) < pdMS_TO_TICKS(SERVICE_NAVIGATION_TIMEOUT_MS)) {
        goto fragment_timeout;
    }

    s_nav_state.active = false;
    publish_navigation_state();
    APP_LOGI(TAG,
             APP_EVT_NAV_TIMEOUT,
             "navigation timed out after %dms last_road=%s",
             SERVICE_NAVIGATION_TIMEOUT_MS,
             s_nav_state.road_name[0] != '\0' ? s_nav_state.road_name : "<empty>");

fragment_timeout:
    if (s_fragment_state.active &&
        (now - s_fragment_state.last_update_tick) >= pdMS_TO_TICKS(NAVIGATION_FRAGMENT_TIMEOUT_MS)) {
        APP_LOGW(TAG,
                 APP_ERR_NAV_PARSE,
                 "drop nav fragments msg=%u progress=%u/%u len=%u timeout=%ums",
                 s_fragment_state.message_id,
                 s_fragment_state.next_chunk_index,
                 s_fragment_state.chunk_count,
                 (unsigned int)s_fragment_state.packet_len,
                 NAVIGATION_FRAGMENT_TIMEOUT_MS);
        reset_fragment_state();
    }
}

static void task_navigation_runtime(void *arg)
{
    (void)arg;
    s_ready = true;
    publish_navigation_state();

    while (true) {
        navigation_command_t command = {0};
        if (xQueueReceive(s_packet_queue, &command, pdMS_TO_TICKS(NAVIGATION_TASK_WAIT_MS)) == pdTRUE) {
            handle_navigation_packet(&command);
        }

        process_navigation_timeout();
    }
}

static esp_err_t ble_submit_navigation_packet(const uint8_t *payload, size_t payload_len)
{
    return service_navigation_submit_packet(payload, payload_len);
}

static esp_err_t ble_submit_obd_packet(const uint8_t *payload, size_t payload_len)
{
    return service_obd_submit_packet(payload, payload_len);
}

esp_err_t service_navigation_init(void)
{
    if (s_packet_queue != NULL && s_task_handle != NULL) {
        return ESP_OK;
    }

    s_packet_queue = xQueueCreate(NAVIGATION_PACKET_QUEUE_LEN, sizeof(navigation_command_t));
    if (s_packet_queue == NULL) {
        APP_LOGE(TAG, APP_ERR_NAV_INIT, "create navigation packet queue failed");
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(task_navigation_runtime,
                                                       "task_navigation",
                                                       NAVIGATION_TASK_STACK,
                                                       NULL,
                                                       NAVIGATION_TASK_PRIORITY,
                                                       &s_task_handle,
                                                       NAVIGATION_TASK_CORE);
    if (created != pdPASS) {
        vQueueDelete(s_packet_queue);
        s_packet_queue = NULL;
        s_task_handle = NULL;
        APP_LOGE(TAG, APP_ERR_NAV_INIT, "create task_navigation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(service_ble_register_bridge_handlers(ble_submit_navigation_packet,
                                                         ble_submit_obd_packet));

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "navigation service ready service=0x%04X nav_char=0x%04X obd_char=0x%04X ble_ready=%d core=%d priority=%d",
             NAVIGATION_SERVICE_UUID16,
             NAVIGATION_CHARACTERISTIC_UUID16,
             OBD_CHARACTERISTIC_UUID16,
             service_ble_is_ready(),
             NAVIGATION_TASK_CORE,
             NAVIGATION_TASK_PRIORITY);
    return ESP_OK;
}

bool service_navigation_is_ready(void)
{
    return s_ready;
}

bool service_navigation_ble_stack_is_active(void)
{
    return service_ble_host_is_active();
}

esp_err_t service_navigation_submit_packet(const uint8_t *packet, size_t packet_len)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_packet_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (is_fragment_frame(packet, packet_len)) {
        return submit_fragment_frame(packet, packet_len);
    }

    if (packet_len < NAVIGATION_PACKET_MIN_SIZE || packet_len > NAVIGATION_PACKET_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    return enqueue_navigation_packet(packet, packet_len);
}

esp_err_t service_navigation_submit_mock_packet(navigation_turn_type_t turn_type,
                                                uint16_t step_distance_m,
                                                uint16_t total_distance_m,
                                                uint16_t remain_time_s,
                                                const char *road_name)
{
    navigation_packet_t packet = {
        .turn_type = (uint8_t)turn_type,
        .step_distance_m = step_distance_m,
        .total_distance_m = total_distance_m,
        .remain_time_s = remain_time_s,
    };
    uint8_t encoded[NAVIGATION_PACKET_MAX_SIZE] = {0};
    size_t encoded_len = 0;

    const esp_err_t road_name_err = navigation_protocol_set_road_name(&packet, road_name);
    if (road_name_err != ESP_OK) {
        return road_name_err;
    }

    const esp_err_t encode_err = navigation_protocol_encode_packet(&packet,
                                                                   encoded,
                                                                   sizeof(encoded),
                                                                   &encoded_len);
    if (encode_err != ESP_OK) {
        return encode_err;
    }

    return service_navigation_submit_packet(encoded, encoded_len);
}
