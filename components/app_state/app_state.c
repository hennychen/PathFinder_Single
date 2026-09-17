#include "app_state.h"

#include <string.h>

#include "app_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_state";
static workflow_state_t s_state;
static ui_page_request_t s_page_request;
static SemaphoreHandle_t s_state_lock;

static const char *page_override_to_string(app_page_override_t override)
{
    switch (override) {
    case APP_PAGE_OVERRIDE_NAVIGATION:
        return "navigation";
    case APP_PAGE_OVERRIDE_OBD_ALERT:
        return "obd_alert";
    case APP_PAGE_OVERRIDE_VOICE:
        return "voice";
    case APP_PAGE_OVERRIDE_NONE:
    default:
        return "none";
    }
}

static void resolve_current_page_locked(void)
{
    const app_page_id_t previous_page = s_state.current_page;
    app_page_id_t resolved_page = s_page_request.selected_page;
    app_page_override_t active_override = APP_PAGE_OVERRIDE_NONE;

    if (s_page_request.navigation_page_active) {
        resolved_page = PAGE_NAV;
        active_override = APP_PAGE_OVERRIDE_NAVIGATION;
    }
    if (s_page_request.obd_alert_page_active) {
        resolved_page = PAGE_OBD;
        active_override = APP_PAGE_OVERRIDE_OBD_ALERT;
    }
    if (s_page_request.voice_page_active) {
        resolved_page = PAGE_VOICE;
        active_override = APP_PAGE_OVERRIDE_VOICE;
    }

    s_page_request.active_override = active_override;
    s_page_request.force_page_active = (active_override != APP_PAGE_OVERRIDE_NONE);
    s_page_request.forced_page = s_page_request.force_page_active ? resolved_page : s_page_request.selected_page;

    if (previous_page == resolved_page) {
        return;
    }

    s_state.previous_page = previous_page;
    s_state.current_page = resolved_page;
    APP_LOGI(TAG,
             APP_EVT_PAGE_CHANGED,
             "page resolved %s -> %s selected=%s override=%s",
             app_pages_to_string(previous_page),
             app_pages_to_string(resolved_page),
             app_pages_to_string(s_page_request.selected_page),
             page_override_to_string(active_override));
}

static void app_state_lock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void app_state_unlock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
}

static uint32_t app_state_now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void app_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.current_page = PAGE_SYSTEM;
    s_state.previous_page = PAGE_SYSTEM;
    s_state.battery_percent = 100;
    s_state.last_user_action_ms = app_state_now_ms();

    memset(&s_page_request, 0, sizeof(s_page_request));
    s_page_request.selected_page = PAGE_SYSTEM;
    s_page_request.forced_page = PAGE_SYSTEM;

    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutex();
    }
}

void app_state_get_snapshot(workflow_state_t *out_state)
{
    if (out_state == NULL) {
        return;
    }

    app_state_lock();
    *out_state = s_state;
    app_state_unlock();
}

void app_state_set_ui_ready(bool ready)
{
    app_state_lock();
    s_state.ui_ready = ready;
    app_state_unlock();
}

void app_state_set_ble_phone_connected(bool connected)
{
    app_state_lock();
    s_state.ble_phone_connected = connected;
    app_state_unlock();
}

void app_state_set_ble_obd_connected(bool connected)
{
    app_state_lock();
    s_state.ble_obd_connected = connected;
    app_state_unlock();
}

void app_state_set_voice_active(bool active)
{
    app_state_lock();
    s_state.voice_active = active;
    s_page_request.voice_page_active = active;
    resolve_current_page_locked();
    app_state_unlock();
}

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
                                 const char *output_text)
{
    app_state_lock();
    s_state.voice_runtime.state_code = state_code;
    s_state.voice_runtime.session_count = session_count;
    s_state.voice_runtime.session_duration_ms = session_duration_ms;
    s_state.voice_runtime.state_duration_ms = state_duration_ms;
    s_state.voice_runtime.input_bytes = input_bytes;
    s_state.voice_runtime.output_bytes = output_bytes;
    s_state.voice_runtime.input_frame_count = input_frame_count;
    s_state.voice_runtime.output_frame_count = output_frame_count;
    s_state.voice_runtime.state_change_count = state_change_count;
    s_state.voice_runtime.output_silence = output_silence;
    if (input_text != NULL) {
        strncpy(s_state.voice_runtime.input_text, input_text, sizeof(s_state.voice_runtime.input_text) - 1U);
        s_state.voice_runtime.input_text[sizeof(s_state.voice_runtime.input_text) - 1U] = '\0';
    } else {
        s_state.voice_runtime.input_text[0] = '\0';
    }
    if (output_text != NULL) {
        strncpy(s_state.voice_runtime.output_text, output_text, sizeof(s_state.voice_runtime.output_text) - 1U);
        s_state.voice_runtime.output_text[sizeof(s_state.voice_runtime.output_text) - 1U] = '\0';
    } else {
        s_state.voice_runtime.output_text[0] = '\0';
    }
    app_state_unlock();
}

void app_state_set_user_page(app_page_id_t page)
{
    if (page >= PAGE_COUNT) {
        return;
    }

    app_state_lock();
    s_page_request.selected_page = page;
    resolve_current_page_locked();
    app_state_unlock();
}

void app_state_set_rtc_time(bool valid,
                            uint16_t year,
                            uint8_t month,
                            uint8_t day,
                            uint8_t hour,
                            uint8_t minute,
                            uint8_t second)
{
    app_state_lock();
    s_state.rtc_valid = valid;
    s_state.rtc_year = year;
    s_state.rtc_month = month;
    s_state.rtc_day = day;
    s_state.rtc_hour = hour;
    s_state.rtc_minute = minute;
    s_state.rtc_second = second;
    app_state_unlock();
}

void app_state_set_attitude(float roll_deg, float pitch_deg, uint16_t heading_deg)
{
    app_state_lock();
    s_state.roll_deg = roll_deg;
    s_state.pitch_deg = pitch_deg;
    s_state.heading_deg = heading_deg;
    app_state_unlock();
}

void app_state_set_navigation_state(const navigation_runtime_state_t *nav_state)
{
    if (nav_state == NULL) {
        return;
    }

    app_state_lock();
    s_state.navigation = *nav_state;
    s_state.nav_active = nav_state->active;
    s_state.last_nav_update_ms = nav_state->last_update_ms;
    s_page_request.navigation_page_active = nav_state->active;
    resolve_current_page_locked();
    app_state_unlock();
}

void app_state_set_obd_state(const obd_runtime_state_t *obd_state)
{
    if (obd_state == NULL) {
        return;
    }

    app_state_lock();
    s_state.obd = *obd_state;
    s_state.obd_connected = obd_state->connected;
    s_state.ble_obd_connected = obd_state->connected;
    s_state.speed_kmh = obd_state->speed_kmh;
    s_state.rpm = obd_state->rpm;
    s_state.coolant_temp_c = obd_state->coolant_temp_c;
    s_page_request.obd_alert_page_active = obd_state->alert_active;
    resolve_current_page_locked();
    app_state_unlock();
}

void app_state_set_audio_state(const audio_runtime_state_t *audio_state)
{
    if (audio_state == NULL) {
        return;
    }

    app_state_lock();
    s_state.audio = *audio_state;
    app_state_unlock();
}

void app_state_set_power_state(const power_runtime_state_t *power_state)
{
    if (power_state == NULL) {
        return;
    }

    app_state_lock();
    s_state.power = *power_state;
    app_state_unlock();
}

void app_state_set_battery_state(uint8_t battery_percent, uint16_t battery_voltage_mv)
{
    app_state_lock();
    s_state.battery_percent = battery_percent;
    s_state.battery_voltage_mv = battery_voltage_mv;
    app_state_unlock();
}

void app_state_mark_user_activity(void)
{
    app_state_lock();
    s_state.last_user_action_ms = app_state_now_ms();
    app_state_unlock();
}

ui_page_request_t app_state_get_page_request(void)
{
    ui_page_request_t request = {0};

    app_state_lock();
    request = s_page_request;
    app_state_unlock();

    return request;
}
