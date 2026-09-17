#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_pages.h"

enum {
    VOICE_RUNTIME_TEXT_MAX = 96,
    APP_STATE_NAVIGATION_ROAD_NAME_MAX = 32,
    OBD_ALERT_TEXT_MAX = 64,
    OBD_PROVIDER_NAME_TEXT_MAX = 24,
    OBD_PROVIDER_STAGE_TEXT_MAX = 24,
    OBD_PROVIDER_DETAIL_TEXT_MAX = 64,
    AUDIO_OWNER_TEXT_MAX = 24,
    AUDIO_POLICY_TEXT_MAX = 32,
};

typedef struct {
    uint8_t state_code;
    uint32_t session_count;
    uint32_t session_duration_ms;
    uint32_t state_duration_ms;
    uint32_t input_bytes;
    uint32_t output_bytes;
    uint32_t input_frame_count;
    uint32_t output_frame_count;
    uint32_t state_change_count;
    bool output_silence;
    char input_text[VOICE_RUNTIME_TEXT_MAX];
    char output_text[VOICE_RUNTIME_TEXT_MAX];
} voice_runtime_state_t;

typedef struct {
    bool active;
    uint8_t turn_type;
    uint16_t step_distance_m;
    uint16_t total_distance_m;
    uint16_t remain_time_s;
    uint32_t packet_count;
    uint32_t invalid_packet_count;
    uint32_t last_update_ms;
    char road_name[APP_STATE_NAVIGATION_ROAD_NAME_MAX + 1];
} navigation_runtime_state_t;

typedef struct {
    bool connected;
    bool alert_active;
    uint16_t speed_kmh;
    uint16_t rpm;
    int8_t coolant_temp_c;
    uint8_t throttle_percent;
    uint8_t fuel_percent;
    uint32_t sample_count;
    uint32_t last_update_ms;
    uint8_t provider_status_code;
    uint32_t provider_connect_attempt_count;
    uint32_t provider_failure_count;
    uint32_t provider_update_count;
    uint32_t provider_ble_scan_elapsed_ms;
    uint32_t provider_ble_connect_elapsed_ms;
    uint32_t provider_ble_discovery_elapsed_ms;
    int32_t provider_ble_last_failure_rc;
    char provider_name[OBD_PROVIDER_NAME_TEXT_MAX];
    char provider_stage[OBD_PROVIDER_STAGE_TEXT_MAX];
    char provider_ble_last_failure_stage[OBD_PROVIDER_STAGE_TEXT_MAX];
    char provider_detail[OBD_PROVIDER_DETAIL_TEXT_MAX];
    char alert_text[OBD_ALERT_TEXT_MAX];
} obd_runtime_state_t;

typedef struct {
    uint8_t owner_code;
    bool mic_requested;
    bool speaker_requested;
    bool mic_enabled;
    bool speaker_enabled;
    bool speaker_muted;
    char owner_text[AUDIO_OWNER_TEXT_MAX];
    char policy_text[AUDIO_POLICY_TEXT_MAX];
} audio_runtime_state_t;

typedef struct {
    bool backlight_dimmed;
    bool light_sleep_ready;
    uint32_t idle_duration_ms;
} power_runtime_state_t;

typedef struct {
    bool ui_ready;
    bool voice_active;
    bool nav_active;
    bool obd_connected;
    bool ble_phone_connected;
    bool ble_obd_connected;
    bool wifi_connected;
    app_page_id_t current_page;
    app_page_id_t previous_page;
    float roll_deg;
    float pitch_deg;
    float imu_temperature_c;
    uint16_t heading_deg;
    uint16_t speed_kmh;
    uint16_t rpm;
    int8_t coolant_temp_c;
    uint8_t battery_percent;
    uint16_t battery_voltage_mv;
    bool rtc_valid;
    uint16_t rtc_year;
    uint8_t rtc_month;
    uint8_t rtc_day;
    uint8_t rtc_hour;
    uint8_t rtc_minute;
    uint8_t rtc_second;
    navigation_runtime_state_t navigation;
    obd_runtime_state_t obd;
    audio_runtime_state_t audio;
    power_runtime_state_t power;
    voice_runtime_state_t voice_runtime;
    uint32_t last_nav_update_ms;
    uint32_t last_user_action_ms;
} workflow_state_t;

typedef enum {
    APP_PAGE_OVERRIDE_NONE = 0,
    APP_PAGE_OVERRIDE_NAVIGATION,
    APP_PAGE_OVERRIDE_OBD_ALERT,
    APP_PAGE_OVERRIDE_VOICE,
} app_page_override_t;

typedef struct {
    app_page_id_t selected_page;
    bool navigation_page_active;
    bool obd_alert_page_active;
    bool voice_page_active;
    bool force_page_active;
    app_page_id_t forced_page;
    app_page_override_t active_override;
} ui_page_request_t;

void app_state_init(void);
void app_state_get_snapshot(workflow_state_t *out_state);
void app_state_set_ui_ready(bool ready);
void app_state_set_ble_phone_connected(bool connected);
void app_state_set_voice_active(bool active);
void app_state_set_voice_runtime(uint8_t state_code,
                                 uint32_t session_count,
                                 uint32_t session_duration_ms,
                                 uint32_t state_duration_ms,
                                 uint32_t input_bytes,
                                 uint32_t output_bytes,
                                 uint32_t input_frame_count,
                                 uint32_t output_frame_count,
                                 uint32_t state_change_count,
                                 bool output_silence,
                                 const char *input_text,
                                 const char *output_text);
void app_state_set_user_page(app_page_id_t page);
void app_state_set_rtc_time(bool valid,
                            uint16_t year,
                            uint8_t month,
                            uint8_t day,
                            uint8_t hour,
                            uint8_t minute,
                            uint8_t second);
void app_state_set_attitude(float roll_deg, float pitch_deg, uint16_t heading_deg);
void app_state_set_imu_temperature(float temperature_c);
void app_state_set_navigation_state(const navigation_runtime_state_t *nav_state);
void app_state_set_ble_obd_connected(bool connected);
void app_state_set_obd_state(const obd_runtime_state_t *obd_state);
void app_state_set_audio_state(const audio_runtime_state_t *audio_state);
void app_state_set_power_state(const power_runtime_state_t *power_state);
void app_state_set_battery_state(uint8_t battery_percent, uint16_t battery_voltage_mv);
void app_state_mark_user_activity(void);
ui_page_request_t app_state_get_page_request(void);
