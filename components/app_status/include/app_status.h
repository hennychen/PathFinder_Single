#pragma once

#include <stdint.h>

#include "esp_log.h"

typedef enum {
    APP_STATUS_OK = 0x0000,

    APP_ERR_BOARD_I2C_INIT = 0x1101,
    APP_ERR_BOARD_EXIO_INIT = 0x1102,
    APP_ERR_BOARD_GPIO_INIT = 0x1103,

    APP_ERR_LCD_INIT = 0x1201,
    APP_ERR_TOUCH_INIT = 0x1202,
    APP_ERR_LVGL_INIT = 0x1203,
    APP_ERR_IMU_INIT = 0x1204,

    APP_ERR_RTC_IO = 0x1301,
    APP_ERR_UI_INIT = 0x1401,
    APP_ERR_ATTITUDE_INIT = 0x1402,
    APP_ERR_VOICE_INIT = 0x1403,
    APP_ERR_AUDIO_INIT = 0x1404,
    APP_ERR_VOICE_BACKEND_INIT = 0x1405,
    APP_ERR_NAV_INIT = 0x1406,
    APP_ERR_NAV_PARSE = 0x1407,
    APP_ERR_OBD_INIT = 0x1408,
    APP_ERR_OBD_PARSE = 0x1409,
    APP_ERR_POWER_INIT = 0x140A,
    APP_ERR_LOG_INIT = 0x140B,

    APP_EVT_PAGE_CHANGED = 0x2001,
    APP_EVT_TOUCH = 0x2002,
    APP_EVT_BOOT_KEY = 0x2003,
    APP_EVT_RTC_SEEDED = 0x2004,
    APP_EVT_VOICE_STATE = 0x2005,
    APP_EVT_VOICE_BACKEND_STATE = 0x2006,
    APP_EVT_NAV_PACKET = 0x2007,
    APP_EVT_NAV_TIMEOUT = 0x2008,
    APP_EVT_OBD_SAMPLE = 0x2009,
    APP_EVT_OBD_ALERT = 0x200A,
    APP_EVT_AUDIO_POLICY = 0x200B,
    APP_EVT_POWER_POLICY = 0x200C,
    APP_EVT_LOG_SINK = 0x200D,
} app_status_code_t;

const char *app_status_to_string(app_status_code_t code);

#define APP_LOGI(tag, code, fmt, ...) \
    ESP_LOGI(tag, "[%s/0x%04X] " fmt, app_status_to_string(code), (unsigned int)(code), ##__VA_ARGS__)

#define APP_LOGW(tag, code, fmt, ...) \
    ESP_LOGW(tag, "[%s/0x%04X] " fmt, app_status_to_string(code), (unsigned int)(code), ##__VA_ARGS__)

#define APP_LOGE(tag, code, fmt, ...) \
    ESP_LOGE(tag, "[%s/0x%04X] " fmt, app_status_to_string(code), (unsigned int)(code), ##__VA_ARGS__)
