#include "service_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_pages.h"
#include "app_state.h"
#include "app_status.h"
#include "board_support.h"
#include "drivers_display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service_voice.h"

static const char *TAG = "service_ui";

enum {
    UI_VOICE_INPUT_ACTIVITY_BYTES = 1280,
    UI_VOICE_OUTPUT_ACTIVITY_BYTES = 1920,
    UI_OBD_DETAIL_SUMMARY_MAX = 48,
};

typedef struct {
    bool tracking;
    uint16_t start_x;
    uint16_t start_y;
    uint16_t last_x;
    uint16_t last_y;
} gesture_state_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_title_label;
static lv_obj_t *s_body_label;
static lv_obj_t *s_footer_label;
static lv_obj_t *s_nav_panel;
static lv_obj_t *s_nav_status_chip;
static lv_obj_t *s_nav_turn_label;
static lv_obj_t *s_nav_arrow_label;
static lv_obj_t *s_nav_distance_label;
static lv_obj_t *s_nav_road_label;
static lv_obj_t *s_nav_detail_label;
static lv_obj_t *s_nav_eta_label;
static lv_obj_t *s_nav_metrics_label;
static lv_obj_t *s_obd_panel;
static lv_obj_t *s_obd_status_chip;
static lv_obj_t *s_obd_speed_value;
static lv_obj_t *s_obd_speed_unit;
static lv_obj_t *s_obd_rpm_value;
static lv_obj_t *s_obd_rpm_unit;
static lv_obj_t *s_obd_coolant_value;
static lv_obj_t *s_obd_throttle_caption;
static lv_obj_t *s_obd_throttle_bar;
static lv_obj_t *s_obd_fuel_caption;
static lv_obj_t *s_obd_fuel_bar;
static lv_obj_t *s_obd_alert_label;
static lv_obj_t *s_obd_metrics_label;
static lv_obj_t *s_voice_panel;
static lv_obj_t *s_voice_state_chip;
static lv_obj_t *s_voice_phase_label;
static lv_obj_t *s_voice_session_label;
static lv_obj_t *s_voice_orb;
static lv_obj_t *s_voice_expression_label;
static lv_obj_t *s_voice_wave_wrap;
static lv_obj_t *s_voice_wave_bars[5];
static lv_obj_t *s_voice_bubble_in;
static lv_obj_t *s_voice_bubble_in_label;
static lv_obj_t *s_voice_bubble_out;
static lv_obj_t *s_voice_bubble_out_label;
static lv_obj_t *s_voice_timeline_label;
static lv_obj_t *s_voice_metrics_label;
static lv_obj_t *s_voice_input_caption;
static lv_obj_t *s_voice_input_bar;
static lv_obj_t *s_voice_output_caption;
static lv_obj_t *s_voice_output_bar;
static lv_obj_t *s_incline_panel;
static lv_obj_t *s_incline_arc_left;
static lv_obj_t *s_incline_arc_right;
static lv_obj_t *s_incline_pitch_slider;
static lv_obj_t *s_incline_roll_label;
static lv_obj_t *s_incline_pitch_label;
static lv_obj_t *s_incline_temp_label;
static lv_obj_t *s_incline_ota_btn;
static lv_obj_t *s_incline_ota_btn_label;
static bool s_incline_ota_requested;
static voice_runtime_state_t s_last_voice_runtime;
static TaskHandle_t s_ui_task;

static uint32_t get_page_refresh_period_ms(app_page_id_t page)
{
    switch (page) {
    case PAGE_ATTITUDE:
        return 50;
    case PAGE_COMPASS:
        return 100;
    case PAGE_INCLINE:
        return 50;
    case PAGE_NAV:
        return 200;
    case PAGE_OBD:
        return 200;
    case PAGE_VOICE:
        return 100;
    case PAGE_SYSTEM:
    default:
        return 500;
    }
}

static bool is_page_switch_locked(void)
{
    const ui_page_request_t request = app_state_get_page_request();
    return request.force_page_active;
}

static const char *voice_state_to_string(uint8_t state_code)
{
    return service_voice_state_to_string((service_voice_state_t)state_code);
}

static const char *voice_phase_text(uint8_t state_code)
{
    switch ((service_voice_state_t)state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "Wake detected";
    case SERVICE_VOICE_STATE_STREAMING:
        return "Listening";
    case SERVICE_VOICE_STATE_SPEAKING:
        return "Speaking";
    case SERVICE_VOICE_STATE_ERROR:
        return "Recovering";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "Ready";
    }
}

static const char *voice_expression_text(const workflow_state_t *state)
{
    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "WAKE";
    case SERVICE_VOICE_STATE_STREAMING:
        return "LISTEN";
    case SERVICE_VOICE_STATE_SPEAKING:
        return state->voice_runtime.output_silence ? "THINK" : "SPEAK";
    case SERVICE_VOICE_STATE_ERROR:
        return "ERROR";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "READY";
    }
}

static const char *voice_input_hint_text(const workflow_state_t *state)
{
    if (state->voice_runtime.input_text[0] != '\0') {
        return state->voice_runtime.input_text;
    }

    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "Wake phrase latched.\nOpening voice session.";
    case SERVICE_VOICE_STATE_STREAMING:
        return (state->voice_runtime.input_bytes > 0U) ? "Mic stream active.\nListening for the next command."
                                                       : "Waiting for voice input.\nStay close to the mic.";
    case SERVICE_VOICE_STATE_SPEAKING:
        return "User side paused.\nMic capture can resume after reply.";
    case SERVICE_VOICE_STATE_ERROR:
        return "Input path interrupted.\nRetrying capture pipeline.";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "Say the wake word.\nSystem is standing by.";
    }
}

static const char *voice_output_hint_text(const workflow_state_t *state)
{
    if (state->voice_runtime.output_text[0] != '\0') {
        return state->voice_runtime.output_text;
    }

    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "Assistant is preparing.\nNo reply yet.";
    case SERVICE_VOICE_STATE_STREAMING:
        return "Speech upload in progress.\nReply is pending.";
    case SERVICE_VOICE_STATE_SPEAKING:
        return state->voice_runtime.output_silence ? "Assistant is thinking.\nTTS audio not emitted yet."
                                                   : "Reply playback active.\nSpeaker output is live.";
    case SERVICE_VOICE_STATE_ERROR:
        return "Output path paused.\nWaiting for backend recovery.";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "Assistant idle.\nReady for the next wake.";
    }
}

static const char *voice_runtime_summary_text(const workflow_state_t *state)
{
    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "Wake captured, bringing up the session";
    case SERVICE_VOICE_STATE_STREAMING:
        return (state->voice_runtime.input_bytes > 0U) ? "Capturing and forwarding user speech"
                                                       : "Listening window open, waiting for speech";
    case SERVICE_VOICE_STATE_SPEAKING:
        return state->voice_runtime.output_silence ? "Assistant is thinking before reply"
                                                   : "Assistant reply is streaming to speaker";
    case SERVICE_VOICE_STATE_ERROR:
        return "Voice pipeline hit an error and is recovering";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "Voice path is idle and ready for wake";
    }
}

static const char *voice_stage_detail_text(const workflow_state_t *state)
{
    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "Wake gate opened and handoff is in progress";
    case SERVICE_VOICE_STATE_STREAMING:
        return "Capture loop is active and frames are feeding upstream";
    case SERVICE_VOICE_STATE_SPEAKING:
        return state->voice_runtime.output_silence ? "Playback state is active but reply payload is still pending"
                                                   : "Speaker frames are being emitted to the DAC path";
    case SERVICE_VOICE_STATE_ERROR:
        return "Runtime fell into recovery and is waiting for the next reset";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "Frontend is armed and waiting for the next wake event";
    }
}

static const char *voice_color_hex_text(uint8_t state_code)
{
    switch ((service_voice_state_t)state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "F5A524";
    case SERVICE_VOICE_STATE_STREAMING:
        return "2ECC71";
    case SERVICE_VOICE_STATE_SPEAKING:
        return "4DA3FF";
    case SERVICE_VOICE_STATE_ERROR:
        return "FF5A5F";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "7F8EA3";
    }
}

static void build_voice_timeline_text(const workflow_state_t *state, char *buffer, size_t buffer_size)
{
    const service_voice_state_t voice_state = (service_voice_state_t)state->voice_runtime.state_code;
    const char *active_hex = voice_color_hex_text(state->voice_runtime.state_code);
    if (voice_state == SERVICE_VOICE_STATE_WAKE_DETECTED) {
        snprintf(buffer,
                 buffer_size,
                 "#%s WAKE# > #4E5D74 LISTEN# > #4E5D74 THINK# > #4E5D74 SPEAK#",
                 active_hex);
    } else if (voice_state == SERVICE_VOICE_STATE_STREAMING) {
        snprintf(buffer,
                 buffer_size,
                 "#4E5D74 WAKE# > #%s LISTEN# > #4E5D74 THINK# > #4E5D74 SPEAK#",
                 active_hex);
    } else if (voice_state == SERVICE_VOICE_STATE_SPEAKING && state->voice_runtime.output_silence) {
        snprintf(buffer,
                 buffer_size,
                 "#4E5D74 WAKE# > #4E5D74 LISTEN# > #%s THINK# > #4E5D74 SPEAK#",
                 active_hex);
    } else if (voice_state == SERVICE_VOICE_STATE_SPEAKING) {
        snprintf(buffer,
                 buffer_size,
                 "#4E5D74 WAKE# > #4E5D74 LISTEN# > #4E5D74 THINK# > #%s SPEAK#",
                 active_hex);
    } else if (voice_state == SERVICE_VOICE_STATE_ERROR) {
        snprintf(buffer,
                 buffer_size,
                 "#FF5A5F RECOVER# > #4E5D74 WAKE# > #4E5D74 LISTEN# > #4E5D74 SPEAK#");
    } else {
        snprintf(buffer,
                 buffer_size,
                 "#7F8EA3 WAKE# > #4E5D74 LISTEN# > #4E5D74 THINK# > #4E5D74 SPEAK#");
    }
}

static lv_color_t voice_state_color(uint8_t state_code)
{
    switch ((service_voice_state_t)state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return lv_color_hex(0xF5A524);
    case SERVICE_VOICE_STATE_STREAMING:
        return lv_color_hex(0x2ECC71);
    case SERVICE_VOICE_STATE_SPEAKING:
        return lv_color_hex(0x4DA3FF);
    case SERVICE_VOICE_STATE_ERROR:
        return lv_color_hex(0xFF5A5F);
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return lv_color_hex(0x7F8EA3);
    }
}

static const char *nav_turn_title(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return "STRAIGHT";
    case 1:
        return "LEFT";
    case 2:
        return "RIGHT";
    case 3:
        return "LEFT FRONT";
    case 4:
        return "RIGHT FRONT";
    case 5:
        return "U-TURN";
    case 6:
        return "ARRIVE";
    default:
        return "UNKNOWN";
    }
}

static const char *nav_turn_chip_text(const workflow_state_t *state)
{
    if (!state->navigation.active) {
        return "NAV IDLE";
    }

    return state->ble_phone_connected ? "PHONE LIVE" : "NAV DEMO";
}

static const char *nav_turn_detail(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return "Keep straight on the current road";
    case 1:
        return "Prepare to turn left";
    case 2:
        return "Prepare to turn right";
    case 3:
        return "Bear left at the next fork";
    case 4:
        return "Bear right at the next fork";
    case 5:
        return "Make a U-turn when possible";
    case 6:
        return "Approaching destination";
    default:
        return "Awaiting a valid navigation frame";
    }
}

static const char *nav_turn_arrow(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return "^^";
    case 1:
        return "<<";
    case 2:
        return ">>";
    case 3:
        return "</";
    case 4:
        return "\\>";
    case 5:
        return "U>";
    case 6:
        return "[]";
    default:
        return "--";
    }
}

static lv_color_t nav_turn_color(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return lv_color_hex(0x4DA3FF);
    case 1:
    case 3:
        return lv_color_hex(0xF5A524);
    case 2:
    case 4:
        return lv_color_hex(0x2ECC71);
    case 5:
        return lv_color_hex(0xFF8A3D);
    case 6:
        return lv_color_hex(0xB56BFF);
    default:
        return lv_color_hex(0x7F8EA3);
    }
}

static const char *obd_status_chip_text(const workflow_state_t *state)
{
    if (!state->obd.connected) {
        if (strcmp(state->obd.provider_stage, "device_not_found") == 0) {
            return "ELM327 MISS";
        }
        if (strcmp(state->obd.provider_stage, "gatt_mismatch") == 0) {
            return "ELM327 GATT";
        }
        if (strcmp(state->obd.provider_stage, "connect_fail") == 0 ||
            strcmp(state->obd.provider_stage, "init_fail") == 0 ||
            strcmp(state->obd.provider_stage, "poll_fail") == 0 ||
            strcmp(state->obd.provider_stage, "response_error") == 0 ||
            strcmp(state->obd.provider_stage, "parse_fail") == 0 ||
            strcmp(state->obd.provider_stage, "write_fail") == 0 ||
            strcmp(state->obd.provider_stage, "invalid_state") == 0) {
            return "ELM327 FAIL";
        }
        return state->obd.provider_connect_attempt_count > 0U ? "ELM327 WAIT" : "ELM327 IDLE";
    }

    return state->obd.alert_active ? "OBD ALERT" : "OBD LIVE";
}

static lv_color_t obd_status_color(const workflow_state_t *state)
{
    if (!state->obd.connected) {
        if (strcmp(state->obd.provider_stage, "device_not_found") == 0) {
            return lv_color_hex(0xA97C50);
        }
        if (strcmp(state->obd.provider_stage, "gatt_mismatch") == 0) {
            return lv_color_hex(0xB56BFF);
        }
        if (strcmp(state->obd.provider_stage, "connect_fail") == 0 ||
            strcmp(state->obd.provider_stage, "init_fail") == 0 ||
            strcmp(state->obd.provider_stage, "poll_fail") == 0 ||
            strcmp(state->obd.provider_stage, "response_error") == 0 ||
            strcmp(state->obd.provider_stage, "parse_fail") == 0 ||
            strcmp(state->obd.provider_stage, "write_fail") == 0 ||
            strcmp(state->obd.provider_stage, "invalid_state") == 0) {
            return lv_color_hex(0xFF8A3D);
        }
        return state->obd.provider_connect_attempt_count > 0U ? lv_color_hex(0xF5A524) : lv_color_hex(0x7F8EA3);
    }

    return state->obd.alert_active ? lv_color_hex(0xFF5A5F) : lv_color_hex(0x2ECC71);
}

static void format_obd_detail_text(const workflow_state_t *state, char *buffer, size_t buffer_size)
{
    const char *detail = NULL;

    if (buffer == NULL || buffer_size == 0U || state == NULL) {
        return;
    }

    if (!state->obd.connected && state->obd.provider_detail[0] != '\0') {
        detail = state->obd.provider_detail;
    } else if (state->obd.alert_text[0] != '\0') {
        detail = state->obd.alert_text;
    } else {
        detail = "Waiting for ELM327 session";
    }

    if (strlen(detail) <= UI_OBD_DETAIL_SUMMARY_MAX) {
        strncpy(buffer, detail, buffer_size - 1U);
        buffer[buffer_size - 1U] = '\0';
        return;
    }

    snprintf(buffer, buffer_size, "%.*s~", UI_OBD_DETAIL_SUMMARY_MAX - 1, detail);
}

static void format_nav_distance(uint16_t distance_m, char *buffer, size_t buffer_size)
{
    if (distance_m >= 1000U) {
        const uint32_t km_int = distance_m / 1000U;
        const uint32_t km_dec = (distance_m % 1000U) / 100U;
        snprintf(buffer, buffer_size, "%lu.%lu km", (unsigned long)km_int, (unsigned long)km_dec);
    } else {
        snprintf(buffer, buffer_size, "%u m", distance_m);
    }
}

static void format_nav_eta(uint16_t remain_time_s, char *buffer, size_t buffer_size)
{
    const uint32_t minutes = remain_time_s / 60U;
    const uint32_t seconds = remain_time_s % 60U;
    if (minutes > 0U) {
        snprintf(buffer, buffer_size, "%lum %02lus", (unsigned long)minutes, (unsigned long)seconds);
    } else {
        snprintf(buffer, buffer_size, "%lus", (unsigned long)seconds);
    }
}

static void set_special_pages_visible(bool voice_visible, bool nav_visible, bool obd_visible, bool incline_visible)
{
    if (s_body_label != NULL) {
        if (voice_visible || nav_visible || obd_visible || incline_visible) {
            lv_obj_add_flag(s_body_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_body_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_incline_panel != NULL) {
        if (incline_visible) {
            lv_obj_clear_flag(s_incline_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_incline_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_voice_panel != NULL) {
        if (voice_visible) {
            lv_obj_clear_flag(s_voice_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_voice_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_nav_panel != NULL) {
        if (nav_visible) {
            lv_obj_clear_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_obd_panel != NULL) {
        if (obd_visible) {
            lv_obj_clear_flag(s_obd_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_obd_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void incline_ota_event_cb(lv_event_t *e)
{
    (void)e;
    // OTA 触发标记：真实固件更新流程由外部 OTA 服务消费该标记
    s_incline_ota_requested = true;
    if (s_incline_ota_btn_label != NULL) {
        lv_label_set_text(s_incline_ota_btn_label, "OTA pending...");
    }
}

// QMI8658 → roll/pitch → LVGL widget update
static void render_incline_page_locked(const workflow_state_t *state)
{
    char roll_buf[24] = {0};
    char pitch_buf[24] = {0};
    char temp_buf[24] = {0};

    // 1. roll 限幅到 ±45° 显示范围
    float roll = state->roll_deg;
    if (roll > 45.0f) {
        roll = 45.0f;
    }
    if (roll < -45.0f) {
        roll = -45.0f;
    }

    // 2. 根据正负值更新对应弧（左弧负角度 / 右弧正角度）
    if (roll >= 0.0f) {
        lv_arc_set_value(s_incline_arc_right, (int16_t)(roll + 0.5f));
        lv_arc_set_value(s_incline_arc_left, 0);
    } else {
        lv_arc_set_value(s_incline_arc_left, (int16_t)(-roll + 0.5f));
        lv_arc_set_value(s_incline_arc_right, 0);
    }

    // 3. 更新中央俯仰滑块（-45..+45）
    float pitch = state->pitch_deg;
    if (pitch > 45.0f) {
        pitch = 45.0f;
    }
    if (pitch < -45.0f) {
        pitch = -45.0f;
    }
    lv_slider_set_value(s_incline_pitch_slider, (int16_t)(pitch + 0.5f), LV_ANIM_OFF);

    // 4. 状态颜色：|angle| > 30° 告警黄，否则正常绿
    const lv_color_t roll_color = (roll > 30.0f || roll < -30.0f) ? lv_color_hex(0xFFCC00) : lv_color_hex(0x00FF00);
    const lv_color_t pitch_color =
        (pitch > 30.0f || pitch < -30.0f) ? lv_color_hex(0xFFCC00) : lv_color_hex(0x00FF00);
    lv_obj_set_style_arc_color(s_incline_arc_left, roll_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_incline_arc_right, roll_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_incline_pitch_slider, pitch_color, LV_PART_INDICATOR);

    // 5. 更新数值标签
    snprintf(roll_buf, sizeof(roll_buf), "R %.1f", (double)state->roll_deg);
    snprintf(pitch_buf, sizeof(pitch_buf), "P %.1f", (double)state->pitch_deg);
    snprintf(temp_buf, sizeof(temp_buf), "%.0f C", (double)state->imu_temperature_c);
    lv_label_set_text(s_incline_roll_label, roll_buf);
    lv_label_set_text(s_incline_pitch_label, pitch_buf);
    lv_label_set_text(s_incline_temp_label, temp_buf);
}

static void render_nav_page_locked(const workflow_state_t *state)
{
    const lv_color_t accent = state->navigation.active ? nav_turn_color(state->navigation.turn_type)
                                                       : lv_color_hex(0x7F8EA3);
    char step_buf[48] = {0};
    char total_buf[48] = {0};
    char eta_buf[48] = {0};
    char footer_buf[128] = {0};
    char detail_buf[256] = {0};
    char metrics_buf[160] = {0};

    format_nav_distance(state->navigation.step_distance_m, step_buf, sizeof(step_buf));
    format_nav_distance(state->navigation.total_distance_m, total_buf, sizeof(total_buf));
    format_nav_eta(state->navigation.remain_time_s, eta_buf, sizeof(eta_buf));

    lv_obj_set_style_bg_color(s_nav_status_chip, accent, 0);
    lv_obj_set_style_bg_color(s_nav_arrow_label, accent, 0);
    lv_obj_set_style_text_color(s_nav_arrow_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(s_nav_turn_label, accent, 0);
    lv_obj_set_style_text_color(s_nav_distance_label, accent, 0);

    lv_label_set_text(s_nav_status_chip, nav_turn_chip_text(state));
    lv_label_set_text(s_nav_turn_label, state->navigation.active ? nav_turn_title(state->navigation.turn_type)
                                                                 : "WAITING ROUTE");
    lv_label_set_text(s_nav_arrow_label, nav_turn_arrow(state->navigation.turn_type));

    if (state->navigation.active) {
        snprintf(detail_buf,
                 sizeof(detail_buf),
                 "%s\nNext maneuver in %s",
                 nav_turn_detail(state->navigation.turn_type),
                 step_buf);
        snprintf(metrics_buf,
                 sizeof(metrics_buf),
                 "Remain %s | ETA %s\nPackets %lu | invalid %lu",
                 total_buf,
                 eta_buf,
                 (unsigned long)state->navigation.packet_count,
                 (unsigned long)state->navigation.invalid_packet_count);
        lv_label_set_text(s_nav_distance_label, step_buf);
        lv_label_set_text(s_nav_road_label,
                          state->navigation.road_name[0] != '\0' ? state->navigation.road_name : "Unnamed road");
    } else {
        snprintf(detail_buf,
                 sizeof(detail_buf),
                 "No active navigation frame yet.\nKeep advertising for phone input.");
        snprintf(metrics_buf,
                 sizeof(metrics_buf),
                 "Last step %s | Packets %lu\nInvalid %lu | BLE %s",
                 step_buf,
                 (unsigned long)state->navigation.packet_count,
                 (unsigned long)state->navigation.invalid_packet_count,
                 state->ble_phone_connected ? "connected" : "idle");
        lv_label_set_text(s_nav_distance_label, "--");
        lv_label_set_text(s_nav_road_label,
                          state->navigation.road_name[0] != '\0' ? state->navigation.road_name : "Waiting for road");
    }

    lv_label_set_text(s_nav_detail_label, detail_buf);
    lv_label_set_text(s_nav_metrics_label, metrics_buf);

    snprintf(footer_buf,
             sizeof(footer_buf),
             "Total %s  |  ETA %s",
             total_buf,
             state->navigation.active ? eta_buf : "--");
    lv_label_set_text(s_nav_eta_label, footer_buf);
}

static void render_obd_page_locked(const workflow_state_t *state)
{
    const lv_color_t accent = obd_status_color(state);
    char speed_buf[32] = {0};
    char rpm_buf[32] = {0};
    char coolant_buf[32] = {0};
    char detail_buf[OBD_ALERT_TEXT_MAX] = {0};
    char metrics_buf[384] = {0};
    char provider_buf[96] = {0};

    lv_obj_set_style_bg_color(s_obd_status_chip, accent, 0);
    lv_obj_set_style_bg_color(s_obd_throttle_bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_obd_fuel_bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_obd_speed_value, accent, 0);
    lv_obj_set_style_text_color(s_obd_rpm_value, accent, 0);
    lv_obj_set_style_text_color(s_obd_coolant_value, state->obd.alert_active ? lv_color_hex(0xFF5A5F) : lv_color_hex(0xD9E4F0), 0);

    lv_label_set_text(s_obd_status_chip, obd_status_chip_text(state));

    snprintf(speed_buf, sizeof(speed_buf), "%u", state->obd.speed_kmh);
    snprintf(rpm_buf, sizeof(rpm_buf), "%u", state->obd.rpm);
    snprintf(coolant_buf, sizeof(coolant_buf), "%d C", state->obd.coolant_temp_c);

    lv_label_set_text(s_obd_speed_value, state->obd.connected ? speed_buf : "--");
    lv_label_set_text(s_obd_rpm_value, state->obd.connected ? rpm_buf : "--");
    lv_label_set_text(s_obd_coolant_value, state->obd.connected ? coolant_buf : "-- C");
    lv_bar_set_value(s_obd_throttle_bar, state->obd.connected ? state->obd.throttle_percent : 0, LV_ANIM_ON);
    lv_bar_set_value(s_obd_fuel_bar, state->obd.connected ? state->obd.fuel_percent : 0, LV_ANIM_ON);

    format_obd_detail_text(state, detail_buf, sizeof(detail_buf));
    lv_label_set_text(s_obd_alert_label, detail_buf);

    snprintf(provider_buf,
             sizeof(provider_buf),
             "%s / %s",
             state->obd.provider_name[0] != '\0' ? state->obd.provider_name : "provider",
             state->obd.provider_stage[0] != '\0' ? state->obd.provider_stage : "idle");
    snprintf(metrics_buf,
             sizeof(metrics_buf),
             "Throttle %u%% | Fuel %u%%\nSamples %lu | Link %s\n%s\nTry %lu | Fail %lu | Upd %lu\nBLE S%lu C%lu D%lu ms\nLast %s rc=%ld",
             state->obd.throttle_percent,
             state->obd.fuel_percent,
             (unsigned long)state->obd.sample_count,
             state->obd.connected ? "live" : "idle",
             provider_buf,
             (unsigned long)state->obd.provider_connect_attempt_count,
             (unsigned long)state->obd.provider_failure_count,
             (unsigned long)state->obd.provider_update_count,
             (unsigned long)state->obd.provider_ble_scan_elapsed_ms,
             (unsigned long)state->obd.provider_ble_connect_elapsed_ms,
             (unsigned long)state->obd.provider_ble_discovery_elapsed_ms,
             state->obd.provider_ble_last_failure_stage[0] != '\0' ? state->obd.provider_ble_last_failure_stage : "idle",
             (long)state->obd.provider_ble_last_failure_rc);
    lv_label_set_text(s_obd_metrics_label, metrics_buf);
}

static void render_voice_page_locked(const workflow_state_t *state)
{
    const uint32_t input_delta = (state->voice_runtime.session_count == s_last_voice_runtime.session_count)
                                     ? (state->voice_runtime.input_bytes - s_last_voice_runtime.input_bytes)
                                     : state->voice_runtime.input_bytes;
    const uint32_t output_delta = (state->voice_runtime.session_count == s_last_voice_runtime.session_count)
                                      ? (state->voice_runtime.output_bytes - s_last_voice_runtime.output_bytes)
                                      : state->voice_runtime.output_bytes;
    const int32_t input_level = LV_MIN(100, (int32_t)((input_delta * 100U) / UI_VOICE_INPUT_ACTIVITY_BYTES));
    const int32_t output_level = LV_MIN(100, (int32_t)((output_delta * 100U) / UI_VOICE_OUTPUT_ACTIVITY_BYTES));
    const int32_t activity_level = LV_MAX(input_level, output_level);
    const lv_color_t accent = voice_state_color(state->voice_runtime.state_code);
    const bool input_active = (state->voice_runtime.state_code == SERVICE_VOICE_STATE_WAKE_DETECTED) ||
                              (state->voice_runtime.state_code == SERVICE_VOICE_STATE_STREAMING);
    const bool output_active = (state->voice_runtime.state_code == SERVICE_VOICE_STATE_SPEAKING);
    char session_buf[64] = {0};
    char timeline_buf[128] = {0};
    char metrics_buf[320] = {0};
    int32_t orb_size = 88;
    int32_t orb_border = 4;
    int32_t wave_levels[5] = {10, 18, 24, 18, 10};
    uint32_t input_rate_bps = 0;
    uint32_t output_rate_bps = 0;

    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
    case SERVICE_VOICE_STATE_STREAMING:
        wave_levels[0] = 10 + (input_level * 12 / 100);
        wave_levels[1] = 18 + (input_level * 18 / 100);
        wave_levels[2] = 24 + (input_level * 22 / 100);
        wave_levels[3] = 18 + (input_level * 18 / 100);
        wave_levels[4] = 10 + (input_level * 12 / 100);
        break;
    case SERVICE_VOICE_STATE_SPEAKING:
        wave_levels[0] = 12 + (output_level * 10 / 100);
        wave_levels[1] = 22 + (output_level * 16 / 100);
        wave_levels[2] = 28 + (output_level * 20 / 100);
        wave_levels[3] = 22 + (output_level * 16 / 100);
        wave_levels[4] = 12 + (output_level * 10 / 100);
        break;
    case SERVICE_VOICE_STATE_ERROR:
        wave_levels[0] = 16;
        wave_levels[1] = 28;
        wave_levels[2] = 16;
        wave_levels[3] = 28;
        wave_levels[4] = 16;
        break;
    case SERVICE_VOICE_STATE_IDLE:
    default:
        break;
    }

    if (state->voice_active) {
        orb_size = 88 + (activity_level / 4);
        orb_border = 4 + (activity_level / 25);
    }

    if (state->voice_runtime.session_duration_ms > 0U) {
        input_rate_bps = (uint32_t)(((uint64_t)state->voice_runtime.input_bytes * 1000ULL) /
                                    state->voice_runtime.session_duration_ms);
        output_rate_bps = (uint32_t)(((uint64_t)state->voice_runtime.output_bytes * 1000ULL) /
                                     state->voice_runtime.session_duration_ms);
    }

    lv_obj_set_style_bg_color(s_voice_state_chip, accent, 0);
    lv_obj_set_style_bg_color(s_voice_orb, accent, 0);
    lv_obj_set_style_border_color(s_voice_orb, accent, 0);
    lv_obj_set_style_border_width(s_voice_orb, orb_border, 0);
    lv_obj_set_size(s_voice_orb, orb_size, orb_size);
    lv_obj_align(s_voice_orb, LV_ALIGN_TOP_LEFT, 20, 64 + ((96 - orb_size) / 2));
    lv_obj_set_style_bg_opa(s_voice_orb, state->voice_active ? LV_OPA_80 : LV_OPA_40, 0);
    lv_obj_set_style_bg_color(s_voice_input_bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_voice_output_bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_voice_bubble_in, input_active ? accent : lv_color_hex(0x2B3A52), 0);
    lv_obj_set_style_border_width(s_voice_bubble_in, input_active ? 2 : 1, 0);
    lv_obj_set_style_bg_color(s_voice_bubble_in, input_active ? lv_color_hex(0x132235) : lv_color_hex(0x101824), 0);
    lv_obj_set_style_text_color(s_voice_bubble_in_label, input_active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xC2D0E0), 0);
    lv_obj_set_style_text_color(s_voice_input_caption, input_active ? accent : lv_color_hex(0x8FA1B8), 0);
    lv_obj_set_style_border_color(s_voice_bubble_out, output_active ? accent : lv_color_hex(0x2B3A52), 0);
    lv_obj_set_style_border_width(s_voice_bubble_out, output_active ? 2 : 1, 0);
    lv_obj_set_style_bg_color(s_voice_bubble_out, output_active ? lv_color_hex(0x132235) : lv_color_hex(0x101824), 0);
    lv_obj_set_style_text_color(s_voice_bubble_out_label, output_active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xC2D0E0), 0);
    lv_obj_set_style_text_color(s_voice_output_caption, output_active ? accent : lv_color_hex(0x8FA1B8), 0);

    lv_label_set_text(s_voice_state_chip, voice_state_to_string(state->voice_runtime.state_code));
    lv_label_set_text(s_voice_phase_label, voice_phase_text(state->voice_runtime.state_code));
    lv_label_set_text(s_voice_expression_label, voice_expression_text(state));
    lv_label_set_text(s_voice_bubble_in_label, voice_input_hint_text(state));
    lv_label_set_text(s_voice_bubble_out_label, voice_output_hint_text(state));
    build_voice_timeline_text(state, timeline_buf, sizeof(timeline_buf));
    lv_label_set_text(s_voice_timeline_label, timeline_buf);

    snprintf(session_buf,
             sizeof(session_buf),
             "Session #%lu  |  %lums",
             (unsigned long)state->voice_runtime.session_count,
             (unsigned long)state->voice_runtime.session_duration_ms);
    lv_label_set_text(s_voice_session_label, session_buf);

    snprintf(metrics_buf,
             sizeof(metrics_buf),
             "%s\n%s\nRX %lu B/s | TX %lu B/s\nFrames in/out %lu / %lu\nState hold %lums | hops %lu\nMode %s",
             voice_runtime_summary_text(state),
             voice_stage_detail_text(state),
             (unsigned long)input_rate_bps,
             (unsigned long)output_rate_bps,
             (unsigned long)state->voice_runtime.input_frame_count,
             (unsigned long)state->voice_runtime.output_frame_count,
             (unsigned long)state->voice_runtime.state_duration_ms,
             (unsigned long)state->voice_runtime.state_change_count,
             state->voice_runtime.output_silence ? "silence" : "payload");
    lv_label_set_text(s_voice_metrics_label, metrics_buf);

    lv_bar_set_value(s_voice_input_bar, state->voice_active ? input_level : 0, LV_ANIM_ON);
    lv_bar_set_value(s_voice_output_bar, state->voice_active ? output_level : 0, LV_ANIM_ON);

    for (size_t i = 0; i < 5; ++i) {
        lv_obj_set_size(s_voice_wave_bars[i], 10, wave_levels[i]);
        lv_obj_align(s_voice_wave_bars[i], LV_ALIGN_BOTTOM_LEFT, (int32_t)(i * 16), 0);
        lv_obj_set_style_bg_color(s_voice_wave_bars[i], accent, 0);
    }

    s_last_voice_runtime = state->voice_runtime;
}

static void render_page_locked(app_page_id_t page, const workflow_state_t *state)
{
    char title_buf[64] = {0};
    char body_buf[320] = {0};
    char footer_buf[96] = {0};

    snprintf(title_buf, sizeof(title_buf), "PathFinder | %s", app_pages_to_string(page));

    switch (page) {
    case PAGE_SYSTEM:
        snprintf(body_buf,
                 sizeof(body_buf),
                 "BOOT OK\n"
                 "%04u-%02u-%02u  %02u:%02u:%02u\n"
                 "UI %s  RTC %s\n"
                 "BAT %u%%  %umV\n"
                 "PHONE BLE %s\n"
                 "OBD BLE %s\n"
                 "AUDIO %s\n"
                 "POLICY %s\n"
                 "IDLE %lu ms\n"
                 "BACKLIGHT %s\n"
                 "SLEEP %s",
                 state->rtc_year,
                 state->rtc_month,
                 state->rtc_day,
                 state->rtc_hour,
                 state->rtc_minute,
                 state->rtc_second,
                 state->ui_ready ? "READY" : "WAIT",
                 state->rtc_valid ? "OK" : "MISS",
                 state->battery_percent,
                 state->battery_voltage_mv,
                 state->ble_phone_connected ? "LIVE" : "IDLE",
                 state->ble_obd_connected ? "LIVE" : "IDLE",
                 state->audio.owner_text[0] != '\0' ? state->audio.owner_text : "none",
                 state->audio.policy_text[0] != '\0' ? state->audio.policy_text : "idle",
                 (unsigned long)state->power.idle_duration_ms,
                 state->power.backlight_dimmed ? "DIM" : "ON",
                 state->power.light_sleep_ready ? "READY" : "WAIT");
        break;
    case PAGE_ATTITUDE:
        snprintf(body_buf,
                 sizeof(body_buf),
                 "Roll: %.1f deg\n"
                 "Pitch: %.1f deg\n"
                 "Heading: %u deg\n"
                 "Source: QMI8658 accel + gyro",
                 (double)state->roll_deg,
                 (double)state->pitch_deg,
                 state->heading_deg);
        break;
    case PAGE_COMPASS:
        snprintf(body_buf,
                 sizeof(body_buf),
                 "Heading: %u deg\n"
                 "Mode: gyro-integrated placeholder\n"
                 "Roll/Pitch: %.1f / %.1f deg",
                 state->heading_deg,
                 (double)state->roll_deg,
                 (double)state->pitch_deg);
        break;
    case PAGE_NAV:
        if (state->navigation.active) {
            snprintf(body_buf,
                     sizeof(body_buf),
                     "Turn: %s\n"
                     "Hint: %s\n"
                     "Next step: %u m\n"
                     "Remain: %u m | %u s\n"
                     "Road: %s\n"
                     "Packets: %lu | invalid: %lu",
                     nav_turn_title(state->navigation.turn_type),
                     nav_turn_detail(state->navigation.turn_type),
                     state->navigation.step_distance_m,
                     state->navigation.total_distance_m,
                     state->navigation.remain_time_s,
                     state->navigation.road_name[0] != '\0' ? state->navigation.road_name : "<no road>",
                     (unsigned long)state->navigation.packet_count,
                     (unsigned long)state->navigation.invalid_packet_count);
        } else {
            snprintf(body_buf,
                     sizeof(body_buf),
                     "Navigation idle\n"
                     "Last road: %s\n"
                     "Last step: %u m\n"
                     "Packets: %lu | invalid: %lu\n"
                     "Waiting for BLE navigation frames",
                     state->navigation.road_name[0] != '\0' ? state->navigation.road_name : "<none>",
                     state->navigation.step_distance_m,
                     (unsigned long)state->navigation.packet_count,
                     (unsigned long)state->navigation.invalid_packet_count);
        }
        break;
    case PAGE_OBD:
        snprintf(body_buf,
                 sizeof(body_buf),
                 "OBD page placeholder\nSpeed: %u km/h\nRPM: %u\nCoolant: %d C",
                 state->speed_kmh,
                 state->rpm,
                 state->coolant_temp_c);
        break;
    case PAGE_VOICE:
        break;
    default:
        snprintf(body_buf, sizeof(body_buf), "Unknown page");
        break;
    }

    snprintf(footer_buf,
             sizeof(footer_buf),
             "Swipe left/right or press BOOT to switch");

    lv_label_set_text(s_title_label, title_buf);
    lv_label_set_text(s_footer_label, footer_buf);

    if (page == PAGE_VOICE) {
        set_special_pages_visible(true, false, false, false);
        render_voice_page_locked(state);
    } else if (page == PAGE_NAV) {
        set_special_pages_visible(false, true, false, false);
        render_nav_page_locked(state);
    } else if (page == PAGE_OBD) {
        set_special_pages_visible(false, false, true, false);
        render_obd_page_locked(state);
    } else if (page == PAGE_INCLINE) {
        set_special_pages_visible(false, false, false, true);
        render_incline_page_locked(state);
    } else {
        set_special_pages_visible(false, false, false, false);
        lv_label_set_text(s_body_label, body_buf);
    }
}

static esp_err_t build_screen_locked(void)
{
    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x050A12), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(s_screen, 24, 0);

    s_title_label = lv_label_create(s_screen);
    s_body_label = lv_label_create(s_screen);
    s_footer_label = lv_label_create(s_screen);
    s_nav_panel = lv_obj_create(s_screen);
    s_nav_status_chip = lv_label_create(s_nav_panel);
    s_nav_turn_label = lv_label_create(s_nav_panel);
    s_nav_arrow_label = lv_label_create(s_nav_panel);
    s_nav_distance_label = lv_label_create(s_nav_panel);
    s_nav_road_label = lv_label_create(s_nav_panel);
    s_nav_detail_label = lv_label_create(s_nav_panel);
    s_nav_eta_label = lv_label_create(s_nav_panel);
    s_nav_metrics_label = lv_label_create(s_nav_panel);
    s_obd_panel = lv_obj_create(s_screen);
    s_obd_status_chip = lv_label_create(s_obd_panel);
    s_obd_speed_value = lv_label_create(s_obd_panel);
    s_obd_speed_unit = lv_label_create(s_obd_panel);
    s_obd_rpm_value = lv_label_create(s_obd_panel);
    s_obd_rpm_unit = lv_label_create(s_obd_panel);
    s_obd_coolant_value = lv_label_create(s_obd_panel);
    s_obd_throttle_caption = lv_label_create(s_obd_panel);
    s_obd_throttle_bar = lv_bar_create(s_obd_panel);
    s_obd_fuel_caption = lv_label_create(s_obd_panel);
    s_obd_fuel_bar = lv_bar_create(s_obd_panel);
    s_obd_alert_label = lv_label_create(s_obd_panel);
    s_obd_metrics_label = lv_label_create(s_obd_panel);
    s_voice_panel = lv_obj_create(s_screen);
    s_voice_state_chip = lv_label_create(s_voice_panel);
    s_voice_phase_label = lv_label_create(s_voice_panel);
    s_voice_session_label = lv_label_create(s_voice_panel);
    s_voice_orb = lv_obj_create(s_voice_panel);
    s_voice_expression_label = lv_label_create(s_voice_panel);
    s_voice_wave_wrap = lv_obj_create(s_voice_panel);
    s_voice_bubble_in = lv_obj_create(s_voice_panel);
    s_voice_bubble_in_label = lv_label_create(s_voice_bubble_in);
    s_voice_bubble_out = lv_obj_create(s_voice_panel);
    s_voice_bubble_out_label = lv_label_create(s_voice_bubble_out);
    s_voice_timeline_label = lv_label_create(s_voice_panel);
    s_voice_metrics_label = lv_label_create(s_voice_panel);
    s_voice_input_caption = lv_label_create(s_voice_panel);
    s_voice_input_bar = lv_bar_create(s_voice_panel);
    s_voice_output_caption = lv_label_create(s_voice_panel);
    s_voice_output_bar = lv_bar_create(s_voice_panel);
    for (size_t i = 0; i < 5; ++i) {
        s_voice_wave_bars[i] = lv_obj_create(s_voice_wave_wrap);
    }
    s_incline_panel = lv_obj_create(s_screen);
    s_incline_arc_left = lv_arc_create(s_incline_panel);
    s_incline_arc_right = lv_arc_create(s_incline_panel);
    s_incline_pitch_slider = lv_slider_create(s_incline_panel);
    s_incline_roll_label = lv_label_create(s_incline_panel);
    s_incline_pitch_label = lv_label_create(s_incline_panel);
    s_incline_temp_label = lv_label_create(s_incline_panel);
    s_incline_ota_btn = lv_btn_create(s_incline_panel);
    s_incline_ota_btn_label = lv_label_create(s_incline_ota_btn);
    if (s_title_label == NULL || s_body_label == NULL || s_footer_label == NULL || s_nav_panel == NULL ||
        s_nav_status_chip == NULL || s_nav_turn_label == NULL || s_nav_arrow_label == NULL ||
        s_nav_distance_label == NULL || s_nav_road_label == NULL || s_nav_detail_label == NULL ||
        s_nav_eta_label == NULL || s_nav_metrics_label == NULL || s_obd_panel == NULL ||
        s_obd_status_chip == NULL || s_obd_speed_value == NULL || s_obd_speed_unit == NULL ||
        s_obd_rpm_value == NULL || s_obd_rpm_unit == NULL || s_obd_coolant_value == NULL ||
        s_obd_throttle_caption == NULL || s_obd_throttle_bar == NULL || s_obd_fuel_caption == NULL ||
        s_obd_fuel_bar == NULL || s_obd_alert_label == NULL || s_obd_metrics_label == NULL ||
        s_voice_panel == NULL ||
        s_voice_state_chip == NULL || s_voice_phase_label == NULL || s_voice_session_label == NULL ||
        s_voice_orb == NULL || s_voice_expression_label == NULL || s_voice_wave_wrap == NULL ||
        s_voice_bubble_in == NULL || s_voice_bubble_in_label == NULL || s_voice_bubble_out == NULL ||
        s_voice_bubble_out_label == NULL || s_voice_timeline_label == NULL || s_voice_metrics_label == NULL ||
        s_voice_input_caption == NULL || s_voice_input_bar == NULL || s_voice_output_caption == NULL ||
        s_voice_output_bar == NULL ||
        s_incline_panel == NULL || s_incline_arc_left == NULL || s_incline_arc_right == NULL ||
        s_incline_pitch_slider == NULL || s_incline_roll_label == NULL || s_incline_pitch_label == NULL ||
        s_incline_temp_label == NULL || s_incline_ota_btn == NULL || s_incline_ota_btn_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < 5; ++i) {
        if (s_voice_wave_bars[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lv_obj_set_width(s_title_label, 360);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_set_width(s_body_label, 332);
    lv_label_set_long_mode(s_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_body_label, LV_ALIGN_TOP_LEFT, 14, 54);
    lv_obj_set_style_text_align(s_body_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_body_label, lv_color_hex(0xD9E4F0), 0);

    lv_obj_set_size(s_nav_panel, 360, 304);
    lv_obj_align(s_nav_panel, LV_ALIGN_CENTER, 0, 8);
    lv_obj_clear_flag(s_nav_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_nav_panel, lv_color_hex(0x0E1624), 0);
    lv_obj_set_style_border_color(s_nav_panel, lv_color_hex(0x1C2B40), 0);
    lv_obj_set_style_radius(s_nav_panel, 24, 0);
    lv_obj_set_style_pad_all(s_nav_panel, 18, 0);
    lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(s_nav_status_chip, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_nav_status_chip, lv_color_hex(0x7F8EA3), 0);
    lv_obj_set_style_bg_opa(s_nav_status_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_nav_status_chip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(s_nav_status_chip, 12, 0);
    lv_obj_set_style_pad_hor(s_nav_status_chip, 12, 0);
    lv_obj_set_style_pad_ver(s_nav_status_chip, 6, 0);
    lv_label_set_text(s_nav_status_chip, "NAV IDLE");

    lv_obj_set_width(s_nav_turn_label, 180);
    lv_obj_align(s_nav_turn_label, LV_ALIGN_TOP_LEFT, 20, 38);
    lv_obj_set_style_text_color(s_nav_turn_label, lv_color_hex(0x7F8EA3), 0);
    lv_label_set_long_mode(s_nav_turn_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_nav_turn_label, "WAITING ROUTE");

    lv_obj_set_size(s_nav_arrow_label, 112, 112);
    lv_obj_align(s_nav_arrow_label, LV_ALIGN_TOP_LEFT, 18, 86);
    lv_obj_set_style_bg_color(s_nav_arrow_label, lv_color_hex(0x7F8EA3), 0);
    lv_obj_set_style_bg_opa(s_nav_arrow_label, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_nav_arrow_label, 56, 0);
    lv_obj_set_style_pad_top(s_nav_arrow_label, 36, 0);
    lv_obj_set_style_pad_bottom(s_nav_arrow_label, 0, 0);
    lv_obj_set_style_pad_left(s_nav_arrow_label, 0, 0);
    lv_obj_set_style_pad_right(s_nav_arrow_label, 0, 0);
    lv_obj_set_style_text_align(s_nav_arrow_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_nav_arrow_label, lv_color_hex(0x7F8EA3), 0);
    lv_label_set_text(s_nav_arrow_label, "--");

    lv_obj_set_width(s_nav_distance_label, 150);
    lv_obj_align(s_nav_distance_label, LV_ALIGN_TOP_RIGHT, -16, 96);
    lv_obj_set_style_text_align(s_nav_distance_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_nav_distance_label, lv_color_hex(0x7F8EA3), 0);
    lv_label_set_text(s_nav_distance_label, "--");

    lv_obj_set_width(s_nav_road_label, 184);
    lv_obj_align(s_nav_road_label, LV_ALIGN_TOP_RIGHT, -16, 132);
    lv_label_set_long_mode(s_nav_road_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_nav_road_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_nav_road_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_nav_road_label, "Waiting for road");

    lv_obj_set_width(s_nav_detail_label, 184);
    lv_obj_align(s_nav_detail_label, LV_ALIGN_TOP_RIGHT, -16, 174);
    lv_label_set_long_mode(s_nav_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_nav_detail_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_nav_detail_label, lv_color_hex(0xC2D0E0), 0);
    lv_label_set_text(s_nav_detail_label, "No active navigation frame yet.");

    lv_obj_set_width(s_nav_eta_label, 320);
    lv_obj_align(s_nav_eta_label, LV_ALIGN_BOTTOM_LEFT, 20, -54);
    lv_obj_set_style_text_color(s_nav_eta_label, lv_color_hex(0xD9E4F0), 0);
    lv_label_set_text(s_nav_eta_label, "Total --  |  ETA --");

    lv_obj_set_width(s_nav_metrics_label, 320);
    lv_obj_align(s_nav_metrics_label, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_label_set_long_mode(s_nav_metrics_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_nav_metrics_label, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_nav_metrics_label, "Packets 0 | Invalid 0");

    lv_obj_set_size(s_obd_panel, 360, 304);
    lv_obj_align(s_obd_panel, LV_ALIGN_CENTER, 0, 8);
    lv_obj_clear_flag(s_obd_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_obd_panel, lv_color_hex(0x0E1624), 0);
    lv_obj_set_style_border_color(s_obd_panel, lv_color_hex(0x1C2B40), 0);
    lv_obj_set_style_radius(s_obd_panel, 24, 0);
    lv_obj_set_style_pad_all(s_obd_panel, 18, 0);
    lv_obj_add_flag(s_obd_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(s_obd_status_chip, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_obd_status_chip, lv_color_hex(0x7F8EA3), 0);
    lv_obj_set_style_bg_opa(s_obd_status_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_obd_status_chip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(s_obd_status_chip, 12, 0);
    lv_obj_set_style_pad_hor(s_obd_status_chip, 12, 0);
    lv_obj_set_style_pad_ver(s_obd_status_chip, 6, 0);
    lv_label_set_text(s_obd_status_chip, "ELM327 IDLE");

    lv_obj_align(s_obd_speed_value, LV_ALIGN_TOP_LEFT, 18, 48);
    lv_obj_set_style_text_color(s_obd_speed_value, lv_color_hex(0x7F8EA3), 0);
    lv_label_set_text(s_obd_speed_value, "--");

    lv_obj_align(s_obd_speed_unit, LV_ALIGN_TOP_LEFT, 18, 98);
    lv_obj_set_style_text_color(s_obd_speed_unit, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_obd_speed_unit, "km/h");

    lv_obj_align(s_obd_rpm_value, LV_ALIGN_TOP_RIGHT, -18, 48);
    lv_obj_set_style_text_align(s_obd_rpm_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_obd_rpm_value, lv_color_hex(0x7F8EA3), 0);
    lv_label_set_text(s_obd_rpm_value, "--");

    lv_obj_align(s_obd_rpm_unit, LV_ALIGN_TOP_RIGHT, -18, 98);
    lv_obj_set_style_text_align(s_obd_rpm_unit, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_obd_rpm_unit, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_obd_rpm_unit, "rpm");

    lv_obj_set_width(s_obd_coolant_value, 320);
    lv_obj_align(s_obd_coolant_value, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_text_align(s_obd_coolant_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_obd_coolant_value, lv_color_hex(0xD9E4F0), 0);
    lv_label_set_text(s_obd_coolant_value, "-- C");

    lv_obj_set_width(s_obd_alert_label, 320);
    lv_obj_align(s_obd_alert_label, LV_ALIGN_TOP_MID, 0, 152);
    lv_label_set_long_mode(s_obd_alert_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_obd_alert_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_obd_alert_label, lv_color_hex(0xD9E4F0), 0);
    lv_label_set_text(s_obd_alert_label, "Waiting for ELM327 session");

    lv_obj_set_width(s_obd_metrics_label, 320);
    lv_obj_align(s_obd_metrics_label, LV_ALIGN_BOTTOM_LEFT, 18, -92);
    lv_label_set_long_mode(s_obd_metrics_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_obd_metrics_label, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_obd_metrics_label,
                      "Throttle 0% | Fuel 0%\nSamples 0 | Link idle\nprovider / idle\nTry 0 | Fail 0 | Upd 0\nBLE S0 C0 D0 ms\nLast idle rc=0");

    lv_obj_align(s_obd_throttle_caption, LV_ALIGN_BOTTOM_LEFT, 18, -72);
    lv_obj_set_style_text_color(s_obd_throttle_caption, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_obd_throttle_caption, "Throttle");

    lv_obj_set_size(s_obd_throttle_bar, 324, 12);
    lv_obj_align(s_obd_throttle_bar, LV_ALIGN_BOTTOM_LEFT, 18, -56);
    lv_bar_set_range(s_obd_throttle_bar, 0, 100);
    lv_obj_set_style_bg_color(s_obd_throttle_bar, lv_color_hex(0x1A2536), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_obd_throttle_bar, lv_color_hex(0x7F8EA3), LV_PART_INDICATOR);

    lv_obj_align(s_obd_fuel_caption, LV_ALIGN_BOTTOM_LEFT, 18, -40);
    lv_obj_set_style_text_color(s_obd_fuel_caption, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_obd_fuel_caption, "Fuel");

    lv_obj_set_size(s_obd_fuel_bar, 324, 12);
    lv_obj_align(s_obd_fuel_bar, LV_ALIGN_BOTTOM_LEFT, 18, -24);
    lv_bar_set_range(s_obd_fuel_bar, 0, 100);
    lv_obj_set_style_bg_color(s_obd_fuel_bar, lv_color_hex(0x1A2536), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_obd_fuel_bar, lv_color_hex(0x7F8EA3), LV_PART_INDICATOR);

    lv_obj_set_size(s_voice_panel, 360, 304);
    lv_obj_align(s_voice_panel, LV_ALIGN_CENTER, 0, 8);
    lv_obj_clear_flag(s_voice_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_voice_panel, lv_color_hex(0x0E1624), 0);
    lv_obj_set_style_border_color(s_voice_panel, lv_color_hex(0x1C2B40), 0);
    lv_obj_set_style_radius(s_voice_panel, 24, 0);
    lv_obj_set_style_pad_all(s_voice_panel, 18, 0);
    lv_obj_add_flag(s_voice_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(s_voice_state_chip, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_voice_state_chip, lv_color_hex(0x7F8EA3), 0);
    lv_obj_set_style_bg_opa(s_voice_state_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_voice_state_chip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(s_voice_state_chip, 12, 0);
    lv_obj_set_style_pad_hor(s_voice_state_chip, 12, 0);
    lv_obj_set_style_pad_ver(s_voice_state_chip, 6, 0);
    lv_label_set_text(s_voice_state_chip, "idle");

    lv_obj_align(s_voice_phase_label, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_color(s_voice_phase_label, lv_color_hex(0xD9E4F0), 0);
    lv_label_set_text(s_voice_phase_label, "Ready");

    lv_obj_align(s_voice_session_label, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_text_color(s_voice_session_label, lv_color_hex(0xA7B6CC), 0);

    lv_obj_set_width(s_voice_timeline_label, 182);
    lv_obj_align(s_voice_timeline_label, LV_ALIGN_TOP_RIGHT, -10, 76);
    lv_label_set_recolor(s_voice_timeline_label, true);
    lv_obj_set_style_text_color(s_voice_timeline_label, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_voice_timeline_label, "#7F8EA3 WAKE# > #4E5D74 LISTEN# > #4E5D74 THINK# > #4E5D74 SPEAK#");

    lv_obj_set_size(s_voice_orb, 96, 96);
    lv_obj_align(s_voice_orb, LV_ALIGN_TOP_LEFT, 20, 64);
    lv_obj_clear_flag(s_voice_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_voice_orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_voice_orb, lv_color_hex(0x7F8EA3), 0);
    lv_obj_set_style_bg_opa(s_voice_orb, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_voice_orb, 4, 0);
    lv_obj_set_style_border_color(s_voice_orb, lv_color_hex(0x7F8EA3), 0);

    lv_obj_align(s_voice_expression_label, LV_ALIGN_TOP_LEFT, 28, 170);
    lv_obj_set_style_text_color(s_voice_expression_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_voice_expression_label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(s_voice_expression_label, "READY");

    lv_obj_set_size(s_voice_wave_wrap, 96, 44);
    lv_obj_align(s_voice_wave_wrap, LV_ALIGN_TOP_LEFT, 20, 196);
    lv_obj_clear_flag(s_voice_wave_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_voice_wave_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_voice_wave_wrap, 0, 0);
    lv_obj_set_style_pad_all(s_voice_wave_wrap, 0, 0);
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_clear_flag(s_voice_wave_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_voice_wave_bars[i], 6, 0);
        lv_obj_set_style_border_width(s_voice_wave_bars[i], 0, 0);
        lv_obj_set_style_bg_color(s_voice_wave_bars[i], lv_color_hex(0x7F8EA3), 0);
        lv_obj_set_size(s_voice_wave_bars[i], 10, 12);
        lv_obj_align(s_voice_wave_bars[i], LV_ALIGN_BOTTOM_LEFT, (int32_t)(i * 16), 0);
    }

    lv_obj_set_size(s_voice_bubble_in, 182, 56);
    lv_obj_align(s_voice_bubble_in, LV_ALIGN_TOP_RIGHT, -10, 96);
    lv_obj_clear_flag(s_voice_bubble_in, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_voice_bubble_in, lv_color_hex(0x101824), 0);
    lv_obj_set_style_border_color(s_voice_bubble_in, lv_color_hex(0x2B3A52), 0);
    lv_obj_set_style_border_width(s_voice_bubble_in, 1, 0);
    lv_obj_set_style_radius(s_voice_bubble_in, 18, 0);
    lv_obj_set_style_pad_all(s_voice_bubble_in, 10, 0);

    lv_obj_set_width(s_voice_bubble_in_label, 158);
    lv_label_set_long_mode(s_voice_bubble_in_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_voice_bubble_in_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(s_voice_bubble_in_label, lv_color_hex(0xC2D0E0), 0);
    lv_label_set_text(s_voice_bubble_in_label, "Say the wake word.\nSystem is standing by.");

    lv_obj_set_size(s_voice_bubble_out, 182, 56);
    lv_obj_align(s_voice_bubble_out, LV_ALIGN_TOP_RIGHT, -10, 160);
    lv_obj_clear_flag(s_voice_bubble_out, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_voice_bubble_out, lv_color_hex(0x101824), 0);
    lv_obj_set_style_border_color(s_voice_bubble_out, lv_color_hex(0x2B3A52), 0);
    lv_obj_set_style_border_width(s_voice_bubble_out, 1, 0);
    lv_obj_set_style_radius(s_voice_bubble_out, 18, 0);
    lv_obj_set_style_pad_all(s_voice_bubble_out, 10, 0);

    lv_obj_set_width(s_voice_bubble_out_label, 158);
    lv_label_set_long_mode(s_voice_bubble_out_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_voice_bubble_out_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(s_voice_bubble_out_label, lv_color_hex(0xC2D0E0), 0);
    lv_label_set_text(s_voice_bubble_out_label, "Assistant idle.\nReady for the next wake.");

    lv_obj_set_width(s_voice_metrics_label, 180);
    lv_label_set_long_mode(s_voice_metrics_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_voice_metrics_label, LV_ALIGN_TOP_RIGHT, -12, 224);
    lv_obj_set_style_text_color(s_voice_metrics_label, lv_color_hex(0xD9E4F0), 0);

    lv_obj_align(s_voice_input_caption, LV_ALIGN_BOTTOM_LEFT, 132, -50);
    lv_obj_set_style_text_color(s_voice_input_caption, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_voice_input_caption, "MIC");

    lv_obj_set_size(s_voice_input_bar, 180, 12);
    lv_obj_align(s_voice_input_bar, LV_ALIGN_BOTTOM_RIGHT, -12, -46);
    lv_bar_set_range(s_voice_input_bar, 0, 100);
    lv_obj_set_style_bg_color(s_voice_input_bar, lv_color_hex(0x1A2536), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_voice_input_bar, lv_color_hex(0x2ECC71), LV_PART_INDICATOR);

    lv_obj_align(s_voice_output_caption, LV_ALIGN_BOTTOM_LEFT, 132, -24);
    lv_obj_set_style_text_color(s_voice_output_caption, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_voice_output_caption, "SPK");

    lv_obj_set_size(s_voice_output_bar, 180, 12);
    lv_obj_align(s_voice_output_bar, LV_ALIGN_BOTTOM_RIGHT, -12, -20);
    lv_bar_set_range(s_voice_output_bar, 0, 100);
    lv_obj_set_style_bg_color(s_voice_output_bar, lv_color_hex(0x1A2536), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_voice_output_bar, lv_color_hex(0x4DA3FF), LV_PART_INDICATOR);

    lv_obj_set_size(s_incline_panel, 360, 304);
    lv_obj_align(s_incline_panel, LV_ALIGN_CENTER, 0, 8);
    lv_obj_clear_flag(s_incline_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_incline_panel, lv_color_hex(0x0E1624), 0);
    lv_obj_set_style_border_color(s_incline_panel, lv_color_hex(0x1C2B40), 0);
    lv_obj_set_style_radius(s_incline_panel, 24, 0);
    lv_obj_set_style_pad_all(s_incline_panel, 18, 0);
    lv_obj_add_flag(s_incline_panel, LV_OBJ_FLAG_HIDDEN);

    // Side-by-side roll arcs: left arc mirrors negative roll, right arc shows positive roll.
    lv_obj_set_size(s_incline_arc_left, 120, 120);
    lv_obj_align(s_incline_arc_left, LV_ALIGN_TOP_LEFT, 8, 30);
    lv_arc_set_rotation(s_incline_arc_left, 0);
    lv_arc_set_bg_angles(s_incline_arc_left, 0, 300);
    lv_arc_set_range(s_incline_arc_left, 0, 45);
    lv_obj_remove_style(s_incline_arc_left, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_incline_arc_left, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_size(s_incline_arc_right, 120, 120);
    lv_obj_align(s_incline_arc_right, LV_ALIGN_TOP_RIGHT, -8, 30);
    lv_arc_set_rotation(s_incline_arc_right, 0);
    lv_arc_set_bg_angles(s_incline_arc_right, 0, 300);
    lv_arc_set_range(s_incline_arc_right, 0, 45);
    lv_obj_remove_style(s_incline_arc_right, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_incline_arc_right, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_size(s_incline_pitch_slider, 120, 16);
    lv_obj_align(s_incline_pitch_slider, LV_ALIGN_BOTTOM_MID, 0, -74);
    lv_slider_set_range(s_incline_pitch_slider, -45, 45);
    lv_obj_clear_flag(s_incline_pitch_slider, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_align(s_incline_roll_label, LV_ALIGN_TOP_LEFT, 22, 158);
    lv_obj_set_style_text_color(s_incline_roll_label, lv_color_hex(0xD9E4F0), 0);
    lv_label_set_text(s_incline_roll_label, "R 0.0");

    lv_obj_align(s_incline_pitch_label, LV_ALIGN_TOP_RIGHT, -22, 158);
    lv_obj_set_style_text_align(s_incline_pitch_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_incline_pitch_label, lv_color_hex(0xD9E4F0), 0);
    lv_label_set_text(s_incline_pitch_label, "P 0.0");

    lv_obj_align(s_incline_temp_label, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_text_color(s_incline_temp_label, lv_color_hex(0x8FA1B8), 0);
    lv_label_set_text(s_incline_temp_label, "-- C");

    lv_obj_set_size(s_incline_ota_btn, 140, 44);
    lv_obj_align(s_incline_ota_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_event_cb(s_incline_ota_btn, incline_ota_event_cb, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(s_incline_ota_btn_label, "OTA Update");

    lv_obj_set_width(s_footer_label, 360);
    lv_label_set_long_mode(s_footer_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_footer_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_color(s_footer_label, lv_color_hex(0xA7B6CC), 0);

    lv_scr_load(s_screen);
    return ESP_OK;
}

static void render_current_page_locked(void)
{
    workflow_state_t state = {0};
    app_state_get_snapshot(&state);
    render_page_locked(state.current_page, &state);
}

static void step_to_page(app_page_id_t target_page)
{
    if (is_page_switch_locked()) {
        return;
    }

    service_ui_show_page(target_page);
}

static void process_boot_key(bool *last_pressed)
{
    const bool pressed = board_support_is_boot_key_pressed();

    if (pressed && !(*last_pressed)) {
        workflow_state_t state = {0};
        app_state_mark_user_activity();
        app_state_get_snapshot(&state);
        APP_LOGI(TAG,
                 APP_EVT_BOOT_KEY,
                 "BOOT key pressed on page=%s",
                 app_pages_to_string(state.current_page));
        step_to_page(app_pages_next(state.current_page));
    }

    *last_pressed = pressed;
}

static void process_touch_gesture(gesture_state_t *gesture)
{
    drivers_display_touch_sample_t sample = {0};
    if (drivers_display_poll_touch(&sample) != ESP_OK) {
        return;
    }

    if (sample.pressed && !gesture->tracking) {
        gesture->tracking = true;
        gesture->start_x = sample.x;
        gesture->start_y = sample.y;
        gesture->last_x = sample.x;
        gesture->last_y = sample.y;
        app_state_mark_user_activity();
        APP_LOGI(TAG,
                 APP_EVT_TOUCH,
                 "touch down x=%u y=%u strength=%u irq=%d",
                 sample.x,
                 sample.y,
                 sample.strength,
                 sample.irq_triggered);
        return;
    }

    if (sample.pressed && gesture->tracking) {
        gesture->last_x = sample.x;
        gesture->last_y = sample.y;
        return;
    }

    if (!sample.pressed && gesture->tracking) {
        const int dx = (int)gesture->last_x - (int)gesture->start_x;
        const int dy = (int)gesture->last_y - (int)gesture->start_y;

        APP_LOGI(TAG,
                 APP_EVT_TOUCH,
                 "touch up start=(%u,%u) end=(%u,%u) dx=%d dy=%d",
                 gesture->start_x,
                 gesture->start_y,
                 gesture->last_x,
                 gesture->last_y,
                 dx,
                 dy);

        if (dx >= 60 && dx > 0 && abs(dx) > abs(dy)) {
            workflow_state_t state = {0};
            app_state_get_snapshot(&state);
            step_to_page(app_pages_prev(state.current_page));
        } else if (dx <= -60 && abs(dx) > abs(dy)) {
            workflow_state_t state = {0};
            app_state_get_snapshot(&state);
            step_to_page(app_pages_next(state.current_page));
        }

        memset(gesture, 0, sizeof(*gesture));
    }
}

static void task_ui_runtime(void *arg)
{
    (void)arg;

    bool last_boot_pressed = false;
    gesture_state_t gesture = {0};
    TickType_t last_render_tick = 0;

    while (true) {
        workflow_state_t state = {0};

        process_boot_key(&last_boot_pressed);
        process_touch_gesture(&gesture);
        app_state_get_snapshot(&state);

        const TickType_t now = xTaskGetTickCount();
        const TickType_t refresh_period = pdMS_TO_TICKS(get_page_refresh_period_ms(state.current_page));
        if (now - last_render_tick >= refresh_period) {
            service_ui_render_active_page();
            last_render_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t service_ui_init(void)
{
    ESP_RETURN_ON_ERROR(drivers_display_init(), TAG, "display stack init failed");

    if (!drivers_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t screen_err = build_screen_locked();
    if (screen_err == ESP_OK) {
        render_current_page_locked();
    }
    drivers_display_unlock();

    if (screen_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_UI_INIT, "build LVGL screen failed: %s", esp_err_to_name(screen_err));
        return screen_err;
    }

    app_state_set_ui_ready(true);
    if (s_ui_task == NULL) {
        xTaskCreatePinnedToCore(task_ui_runtime, "task_ui_runtime", 6144, NULL, 4, &s_ui_task, 1);
    }

    APP_LOGI(TAG, APP_STATUS_OK, "UI service initialized with LVGL");
    service_ui_show_page(PAGE_SYSTEM);
    return ESP_OK;
}

void service_ui_show_page(app_page_id_t page)
{
    if (is_page_switch_locked() && page != PAGE_VOICE) {
        return;
    }

    app_state_set_user_page(page);
    service_ui_render_active_page();
}

void service_ui_render_system_page(void)
{
    workflow_state_t state = {0};
    app_state_get_snapshot(&state);

    if (state.current_page != PAGE_SYSTEM) {
        return;
    }

    service_ui_render_active_page();
}

void service_ui_render_active_page(void)
{
    if (!drivers_display_is_ready()) {
        return;
    }

    if (!drivers_display_lock(0)) {
        return;
    }

    render_current_page_locked();
    drivers_display_unlock();
}
