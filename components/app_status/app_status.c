#include "app_status.h"

const char *app_status_to_string(app_status_code_t code)
{
    switch (code) {
    case APP_STATUS_OK:
        return "ok";
    case APP_ERR_BOARD_I2C_INIT:
        return "board_i2c_init_failed";
    case APP_ERR_BOARD_EXIO_INIT:
        return "board_exio_init_failed";
    case APP_ERR_BOARD_GPIO_INIT:
        return "board_gpio_init_failed";
    case APP_ERR_LCD_INIT:
        return "lcd_init_failed";
    case APP_ERR_TOUCH_INIT:
        return "touch_init_failed";
    case APP_ERR_LVGL_INIT:
        return "lvgl_init_failed";
    case APP_ERR_IMU_INIT:
        return "imu_init_failed";
    case APP_ERR_RTC_IO:
        return "rtc_io_failed";
    case APP_ERR_UI_INIT:
        return "ui_init_failed";
    case APP_ERR_ATTITUDE_INIT:
        return "attitude_init_failed";
    case APP_ERR_VOICE_INIT:
        return "voice_init_failed";
    case APP_ERR_AUDIO_INIT:
        return "audio_init_failed";
    case APP_ERR_VOICE_BACKEND_INIT:
        return "voice_backend_init_failed";
    case APP_ERR_NAV_INIT:
        return "nav_init_failed";
    case APP_ERR_NAV_PARSE:
        return "nav_parse_failed";
    case APP_ERR_OBD_INIT:
        return "obd_init_failed";
    case APP_ERR_OBD_PARSE:
        return "obd_parse_failed";
    case APP_ERR_POWER_INIT:
        return "power_init_failed";
    case APP_ERR_LOG_INIT:
        return "log_init_failed";
    case APP_EVT_PAGE_CHANGED:
        return "page_changed";
    case APP_EVT_TOUCH:
        return "touch_event";
    case APP_EVT_BOOT_KEY:
        return "boot_key_event";
    case APP_EVT_RTC_SEEDED:
        return "rtc_seeded";
    case APP_EVT_VOICE_STATE:
        return "voice_state";
    case APP_EVT_VOICE_BACKEND_STATE:
        return "voice_backend_state";
    case APP_EVT_NAV_PACKET:
        return "nav_packet";
    case APP_EVT_NAV_TIMEOUT:
        return "nav_timeout";
    case APP_EVT_OBD_SAMPLE:
        return "obd_sample";
    case APP_EVT_OBD_ALERT:
        return "obd_alert";
    case APP_EVT_AUDIO_POLICY:
        return "audio_policy";
    case APP_EVT_POWER_POLICY:
        return "power_policy";
    case APP_EVT_LOG_SINK:
        return "log_sink";
    default:
        return "unknown_status";
    }
}
