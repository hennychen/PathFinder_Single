#include "app_status.h"
#include "board_support.h"
#include "pcf85063.h"
#include "qmi8658.h"

#ifndef PATHFINDER_QMI_MIN_BRINGUP
#include "app_pages.h"
#include "app_state.h"
#include "service_attitude.h"
#include "service_audio.h"
#include "service_ble.h"
#include "service_navigation.h"
#include "service_obd.h"
#include "service_power.h"
#include "service_log.h"
#include "service_ui.h"
#include "service_voice.h"
#endif

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

#ifdef PATHFINDER_QMI_MIN_BRINGUP
#include "driver/gpio.h"

// #region debug-point qmi-min-bringup:line-observer
// Core-1 observer: when the core-0 bring-up path hangs inside an I2C
// transaction, this task keeps running and samples the shared I2C lines
// (SCL=GPIO10, SDA=GPIO11) to classify the hang:
//   sda=0 scl=1 -> a slave is wedging SDA low
//   sda=0 scl=0 -> bus held low / clock stretch stuck
//   sda=1 scl=1 -> bus idle, I2C driver/controller wedged while lines released
static void task_qmi_line_observer(void *arg)
{
    (void)arg;
    const board_pins_t *pins = board_support_get_pins();
    int last_phase = -1;
    int phase_hold_ms = 0;
    int report_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const int phase = qmi8658_get_min_bringup_phase();

        if (phase != last_phase) {
            if (last_phase != -1) {
                APP_LOGW(TAG,
                         APP_STATUS_OK,
                         "line-observer: phase %d(%s) -> %d(%s) after %dms",
                         last_phase,
                         qmi8658_get_min_bringup_phase_name(last_phase),
                         phase,
                         qmi8658_get_min_bringup_phase_name(phase),
                         phase_hold_ms);
            }
            last_phase = phase;
            phase_hold_ms = 0;
            report_count = 0;
            continue;
        }

        phase_hold_ms += 100;
        const int hang_threshold_ms = qmi8658_min_bringup_in_progress() ? 2000 : 6000;
        if (phase_hold_ms < hang_threshold_ms || (phase_hold_ms % 1000) != 0) {
            continue;
        }

        const int scl = gpio_get_level(pins->i2c_scl_gpio);
        const int sda = gpio_get_level(pins->i2c_sda_gpio);
        APP_LOGW(TAG,
                 APP_STATUS_OK,
                 "line-observer: HUNG phase=%d(%s) hold=%dms scl(io%d)=%d sda(io%d)=%d report#%d",
                 phase,
                 qmi8658_get_min_bringup_phase_name(phase),
                 phase_hold_ms,
                 pins->i2c_scl_gpio,
                 scl,
                 pins->i2c_sda_gpio,
                 sda,
                 ++report_count);
    }
}
// #endregion debug-point qmi-min-bringup:line-observer
#endif

#ifndef PATHFINDER_QMI_MIN_BRINGUP
static const pcf85063_time_t k_rtc_seed_time = {
    .year = 2026,
    .month = 1,
    .day = 1,
    .weekday = 4,
    .hour = 0,
    .minute = 0,
    .second = 0,
};

static void refresh_rtc_state(void)
{
    pcf85063_time_t rtc_time = {0};
    const esp_err_t err = pcf85063_read_time(&rtc_time);
    if (err != ESP_OK) {
        app_state_set_rtc_time(false, 0, 0, 0, 0, 0, 0);
        APP_LOGW(TAG, APP_ERR_RTC_IO, "RTC read failed: %s", esp_err_to_name(err));
        return;
    }

    app_state_set_rtc_time(true,
                           rtc_time.year,
                           rtc_time.month,
                           rtc_time.day,
                           rtc_time.hour,
                           rtc_time.minute,
                           rtc_time.second);
}

static void seed_rtc_if_needed(void)
{
    pcf85063_time_t rtc_time = {0};
    const esp_err_t read_err = pcf85063_read_time(&rtc_time);
    if (read_err == ESP_OK && pcf85063_is_time_valid(&rtc_time)) {
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "RTC read valid time %04u-%02u-%02u %02u:%02u:%02u",
                 rtc_time.year,
                 rtc_time.month,
                 rtc_time.day,
                 rtc_time.hour,
                 rtc_time.minute,
                 rtc_time.second);
        return;
    }

    const esp_err_t write_err = pcf85063_set_time(&k_rtc_seed_time);
    if (write_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_RTC_IO, "RTC seed write failed: %s", esp_err_to_name(write_err));
        return;
    }

    APP_LOGI(TAG,
             APP_EVT_RTC_SEEDED,
             "RTC seeded to %04u-%02u-%02u %02u:%02u:%02u",
             k_rtc_seed_time.year,
             k_rtc_seed_time.month,
             k_rtc_seed_time.day,
             k_rtc_seed_time.hour,
             k_rtc_seed_time.minute,
             k_rtc_seed_time.second);
}

static void task_system_heartbeat(void *arg)
{
    (void)arg;

    while (true) {
        refresh_rtc_state();

        workflow_state_t state = {0};
        app_state_get_snapshot(&state);

        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "heartbeat | page=%s ui_ready=%d voice=%d nav=%d obd=%d bat=%u%%/%umV audio=%s idle=%lums backlight=%d rtc_valid=%d",
                 app_pages_to_string(state.current_page),
                 state.ui_ready,
                 state.voice_active,
                 state.nav_active,
                 state.obd_connected,
                 state.battery_percent,
                 state.battery_voltage_mv,
                 state.audio.owner_text[0] != '\0' ? state.audio.owner_text : "none",
                 (unsigned long)state.power.idle_duration_ms,
                 !state.power.backlight_dimmed,
                 state.rtc_valid);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif

void app_main(void)
{
#ifdef PATHFINDER_QMI_MIN_BRINGUP
    APP_LOGW(TAG, APP_STATUS_OK, "QMI8658 minimal bring-up mode enabled");
    // Start the core-1 observer before any I2C activity so a hang during
    // bring-up can still be classified from line levels.
    xTaskCreatePinnedToCore(task_qmi_line_observer,
                            "qmi_line_observer",
                            3072,
                            NULL,
                            2,
                            NULL,
                            1);
    ESP_ERROR_CHECK(board_support_init());

    const esp_err_t rtc_init_err = pcf85063_init();
    APP_LOGW(TAG, APP_STATUS_OK, "minimal bring-up rtc probe result: %s", esp_err_to_name(rtc_init_err));

    const esp_err_t qmi_min_err = qmi8658_run_minimal_bringup();
    APP_LOGW(TAG, APP_STATUS_OK, "minimal bring-up qmi result: %s", esp_err_to_name(qmi_min_err));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    app_state_init();
    ESP_ERROR_CHECK(board_support_init());
    const esp_err_t log_init_err = service_log_init();
    if (log_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_LOG_INIT, "log service skipped: %s", esp_err_to_name(log_init_err));
    }

    APP_LOGI(TAG, APP_STATUS_OK, "PathFinder Single starting");

    const esp_err_t rtc_init_err = pcf85063_init();
    if (rtc_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_RTC_IO, "RTC init skipped: %s", esp_err_to_name(rtc_init_err));
    } else {
        seed_rtc_if_needed();
        refresh_rtc_state();
    }
    const esp_err_t attitude_init_err = service_attitude_init();
    if (attitude_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_ATTITUDE_INIT, "attitude service skipped: %s", esp_err_to_name(attitude_init_err));
    }
    const esp_err_t audio_init_err = service_audio_init();
    if (audio_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_AUDIO_INIT, "audio service skipped: %s", esp_err_to_name(audio_init_err));
    }
    const esp_err_t voice_init_err = service_voice_init();
    if (voice_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_VOICE_INIT, "voice service skipped: %s", esp_err_to_name(voice_init_err));
    }
    const esp_err_t navigation_init_err = service_navigation_init();
    if (navigation_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "navigation service skipped: %s", esp_err_to_name(navigation_init_err));
    } else {
        const esp_err_t ble_init_err = service_ble_init();
        if (ble_init_err != ESP_OK) {
            APP_LOGW(TAG, APP_ERR_NAV_INIT, "shared ble service skipped: %s", esp_err_to_name(ble_init_err));
        }
    }
    const esp_err_t obd_init_err = service_obd_init();
    if (obd_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_OBD_INIT, "obd service skipped: %s", esp_err_to_name(obd_init_err));
    }
    ESP_ERROR_CHECK(service_ui_init());
    const esp_err_t power_init_err = service_power_init();
    if (power_init_err != ESP_OK) {
        APP_LOGW(TAG, APP_ERR_POWER_INIT, "power service skipped: %s", esp_err_to_name(power_init_err));
    }

    xTaskCreatePinnedToCore(task_system_heartbeat,
                            "task_system_heartbeat",
                            4096,
                            NULL,
                            1,
                            NULL,
                            0);
#endif
}
